#include <wctype.h>
#include "edit.h"
#include "buffer.h"
#include "change.h"
#include "indent.h"
#include "move.h"
#include "regexp.h"
#include "selection.h"
#include "util/string.h"
#include "util/utf8.h"
#include "util/xmalloc.h"
#include "view.h"

typedef struct {
    String buf;
    char *indent;
    size_t indent_len;
    size_t indent_width;
    size_t cur_width;
    size_t text_width;
} ParagraphFormatter;

static char *copy_buf;
static size_t copy_len;
static bool copy_is_lines;

static const char spattern[] = "\\{[ \t]*(//.*|/\\*.*\\*/[ \t]*)?$";
static const char epattern[] = "^[ \t]*\\}";

/*
 * Stupid { ... } block selector.
 *
 * Because braces can be inside strings or comments and writing real
 * parser for many programming languages does not make sense the rules
 * for selecting a block are made very simple. Line that matches \{\s*$
 * starts a block and line that matches ^\s*\} ends it.
 */
void select_block(void)
{
    BlockIter sbi, ebi, bi = view->cursor;
    LineRef lr;
    int level = 0;

    // If current line does not match \{\s*$ but matches ^\s*\} then
    // cursor is likely at end of the block you want to select.
    fetch_this_line(&bi, &lr);
    if (
        !regexp_match_nosub(spattern, lr.line, lr.size)
        && regexp_match_nosub(epattern, lr.line, lr.size)
    ) {
        block_iter_prev_line(&bi);
    }

    while (1) {
        fetch_this_line(&bi, &lr);
        if (regexp_match_nosub(spattern, lr.line, lr.size)) {
            if (level++ == 0) {
                sbi = bi;
                block_iter_next_line(&bi);
                break;
            }
        }
        if (regexp_match_nosub(epattern, lr.line, lr.size)) {
            level--;
        }

        if (!block_iter_prev_line(&bi)) {
            return;
        }
    }

    while (1) {
        fetch_this_line(&bi, &lr);
        if (regexp_match_nosub(epattern, lr.line, lr.size)) {
            if (--level == 0) {
                ebi = bi;
                break;
            }
        }
        if (regexp_match_nosub(spattern, lr.line, lr.size)) {
            level++;
        }

        if (!block_iter_next_line(&bi)) {
            return;
        }
    }

    view->cursor = sbi;
    view->sel_so = block_iter_get_offset(&ebi);
    view->sel_eo = UINT_MAX;
    view->selection = SELECT_LINES;

    mark_all_lines_changed(buffer);
}

static int get_indent_of_matching_brace(void)
{
    BlockIter bi = view->cursor;
    LineRef lr;
    int level = 0;

    while (block_iter_prev_line(&bi)) {
        fetch_this_line(&bi, &lr);
        if (regexp_match_nosub(spattern, lr.line, lr.size)) {
            if (level++ == 0) {
                IndentInfo info;
                get_indent_info(lr.line, lr.size, &info);
                return info.width;
            }
        }
        if (regexp_match_nosub(epattern, lr.line, lr.size)) {
            level--;
        }
    }
    return -1;
}

void unselect(void)
{
    if (view->selection) {
        view->selection = SELECT_NONE;
        mark_all_lines_changed(buffer);
    }
}

static void record_copy(char *buf, size_t len, bool is_lines)
{
    if (copy_buf) {
        free(copy_buf);
    }
    copy_buf = buf;
    copy_len = len;
    copy_is_lines = is_lines;
}

void cut(size_t len, bool is_lines)
{
    if (len) {
        char *buf = block_iter_get_bytes(&view->cursor, len);
        record_copy(buf, len, is_lines);
        buffer_delete_bytes(len);
    }
}

void copy(size_t len, bool is_lines)
{
    if (len) {
        char *buf = block_iter_get_bytes(&view->cursor, len);
        record_copy(buf, len, is_lines);
    }
}

void insert_text(const char *text, size_t size)
{
    size_t del_count = 0;

    if (view->selection) {
        del_count = prepare_selection(view);
        unselect();
    }
    buffer_replace_bytes(del_count, text, size);
    block_iter_skip_bytes(&view->cursor, size);
}

void paste(bool at_cursor)
{
    if (!copy_buf) {
        return;
    }

    size_t del_count = 0;
    if (view->selection) {
        del_count = prepare_selection(view);
        unselect();
    }

    if (copy_is_lines && !at_cursor) {
        const long x = view_get_preferred_x(view);
        if (!del_count) {
            block_iter_eat_line(&view->cursor);
        }
        buffer_replace_bytes(del_count, copy_buf, copy_len);

        // Try to keep cursor column
        move_to_preferred_x(x);
        // New preferred_x
        view_reset_preferred_x(view);
    } else {
        buffer_replace_bytes(del_count, copy_buf, copy_len);
    }
}

void delete_ch(void)
{
    size_t size = 0;
    if (view->selection) {
        size = prepare_selection(view);
        unselect();
    } else {
        begin_change(CHANGE_MERGE_DELETE);
        if (buffer->options.emulate_tab) {
            size = get_indent_level_bytes_right();
        }
        if (size == 0) {
            BlockIter bi = view->cursor;
            size = block_iter_next_column(&bi);
        }
    }
    buffer_delete_bytes(size);
}

void erase(void)
{
    size_t size = 0;
    if (view->selection) {
        size = prepare_selection(view);
        unselect();
    } else {
        begin_change(CHANGE_MERGE_ERASE);
        if (buffer->options.emulate_tab) {
            size = get_indent_level_bytes_left();
            block_iter_back_bytes(&view->cursor, size);
        }
        if (size == 0) {
            CodePoint u;
            size = block_iter_prev_char(&view->cursor, &u);
        }
    }
    buffer_erase_bytes(size);
}

// Go to beginning of whitespace (tabs and spaces) under cursor and
// return number of whitespace bytes after cursor after moving cursor
static size_t goto_beginning_of_whitespace(void)
{
    BlockIter bi = view->cursor;
    size_t count = 0;
    CodePoint u;

    // Count spaces and tabs at or after cursor
    while (block_iter_next_char(&bi, &u)) {
        if (u != '\t' && u != ' ') {
            break;
        }
        count++;
    }

    // Count spaces and tabs before cursor
    while (block_iter_prev_char(&view->cursor, &u)) {
        if (u != '\t' && u != ' ') {
            block_iter_next_char(&view->cursor, &u);
            break;
        }
        count++;
    }
    return count;
}

static bool ws_only(const LineRef *lr)
{
    for (size_t i = 0, n = lr->size; i < n; i++) {
        char ch = lr->line[i];
        if (ch != ' ' && ch != '\t') {
            return false;
        }
    }
    return true;
}

// Non-empty line can be used to determine size of indentation for the next line
static bool find_non_empty_line_bwd(BlockIter *bi)
{
    block_iter_bol(bi);
    do {
        LineRef lr;
        fill_line_ref(bi, &lr);
        if (!ws_only(&lr)) {
            return true;
        }
    } while (block_iter_prev_line(bi));
    return false;
}

static void insert_nl(void)
{
    size_t del_count = 0;
    size_t ins_count = 1;
    char *ins = NULL;

    // Prepare deleted text (selection or whitespace around cursor)
    if (view->selection) {
        del_count = prepare_selection(view);
        unselect();
    } else {
        // Trim whitespace around cursor
        del_count = goto_beginning_of_whitespace();
    }

    // Prepare inserted indentation
    if (buffer->options.auto_indent) {
        // Current line will be split at cursor position
        BlockIter bi = view->cursor;
        size_t len = block_iter_bol(&bi);
        LineRef lr;

        fill_line_ref(&bi, &lr);
        lr.size = len;
        if (ws_only(&lr)) {
            // This line is (or will become) white space only.
            // Find previous non whitespace only line.
            if (block_iter_prev_line(&bi) && find_non_empty_line_bwd(&bi)) {
                fill_line_ref(&bi, &lr);
                ins = get_indent_for_next_line(lr.line, lr.size);
            }
        } else {
            ins = get_indent_for_next_line(lr.line, lr.size);
        }
    }

    begin_change(CHANGE_MERGE_NONE);
    if (ins) {
        // Add newline before indent
        ins_count = strlen(ins);
        memmove(ins + 1, ins, ins_count);
        ins[0] = '\n';
        ins_count++;

        buffer_replace_bytes(del_count, ins, ins_count);
        free(ins);
    } else {
        buffer_replace_bytes(del_count, "\n", ins_count);
    }
    end_change();

    // Move after inserted text
    block_iter_skip_bytes(&view->cursor, ins_count);
}

void insert_ch(CodePoint ch)
{
    size_t del_count = 0;
    size_t ins_count = 0;

    if (ch == '\n') {
        insert_nl();
        return;
    }

    char *ins = xmalloc(8);
    if (view->selection) {
        // Prepare deleted text (selection)
        del_count = prepare_selection(view);
        unselect();
    } else if (
        ch == '}'
        && buffer->options.auto_indent
        && buffer->options.brace_indent
    ) {
        BlockIter bi = view->cursor;
        LineRef curlr;

        block_iter_bol(&bi);
        fill_line_ref(&bi, &curlr);
        if (ws_only(&curlr)) {
            int width = get_indent_of_matching_brace();

            if (width >= 0) {
                // Replace current (ws only) line with some indent + '}'
                block_iter_bol(&view->cursor);
                del_count = curlr.size;
                if (width) {
                    free(ins);
                    ins = make_indent(width);
                    ins_count = strlen(ins);
                    // '}' will be replace the terminating NUL
                }
            }
        }
    }

    // Prepare inserted text
    if (ch == '\t' && buffer->options.expand_tab) {
        ins_count = buffer->options.indent_width;
        memset(ins, ' ', ins_count);
    } else {
        u_set_char_raw(ins, &ins_count, ch);
    }

    // Record change
    if (del_count) {
        begin_change(CHANGE_MERGE_NONE);
    } else {
        begin_change(CHANGE_MERGE_INSERT);
    }
    buffer_replace_bytes(del_count, ins, ins_count);
    end_change();

    // Move after inserted text
    block_iter_skip_bytes(&view->cursor, ins_count);

    free(ins);
}

static void join_selection(void)
{
    size_t count = prepare_selection(view);
    size_t len = 0, join = 0;
    BlockIter bi;
    CodePoint ch = 0;

    unselect();
    bi = view->cursor;

    begin_change_chain();
    while (count > 0) {
        if (!len) {
            view->cursor = bi;
        }

        count -= block_iter_next_char(&bi, &ch);
        if (ch == '\t' || ch == ' ') {
            len++;
        } else if (ch == '\n') {
            len++;
            join++;
        } else {
            if (join) {
                buffer_replace_bytes(len, " ", 1);
                // Skip the space we inserted and the char we read last
                block_iter_next_char(&view->cursor, &ch);
                block_iter_next_char(&view->cursor, &ch);
                bi = view->cursor;
            }
            len = 0;
            join = 0;
        }
    }

    // Don't replace last \n that is at end of the selection
    if (join && ch == '\n') {
        join--;
        len--;
    }

    if (join) {
        if (ch == '\n') {
            // Don't add space to end of line
            buffer_delete_bytes(len);
        } else {
            buffer_replace_bytes(len, " ", 1);
        }
    }
    end_change_chain();
}

void join_lines(void)
{
    BlockIter bi = view->cursor;

    if (view->selection) {
        join_selection();
        return;
    }

    if (!block_iter_next_line(&bi)) {
        return;
    }
    if (block_iter_is_eof(&bi)) {
        return;
    }

    BlockIter next = bi;
    CodePoint u;
    size_t count = 1;
    block_iter_prev_char(&bi, &u);
    while (block_iter_prev_char(&bi, &u)) {
        if (u != '\t' && u != ' ') {
            block_iter_next_char(&bi, &u);
            break;
        }
        count++;
    }
    while (block_iter_next_char(&next, &u)) {
        if (u != '\t' && u != ' ') {
            break;
        }
        count++;
    }

    view->cursor = bi;
    if (u == '\n') {
        buffer_delete_bytes(count);
    } else {
        buffer_replace_bytes(count, " ", 1);
    }
}

static void shift_right(size_t nr_lines, size_t count)
{
    size_t indent_size;
    char *indent;

    indent = alloc_indent(count, &indent_size);
    size_t i = 0;
    while (1) {
        IndentInfo info;
        LineRef lr;

        fetch_this_line(&view->cursor, &lr);
        get_indent_info(lr.line, lr.size, &info);
        if (info.wsonly) {
            if (info.bytes) {
                // Remove indentation
                buffer_delete_bytes(info.bytes);
            }
        } else if (info.sane) {
            // Insert whitespace
            buffer_insert_bytes(indent, indent_size);
        } else {
            // Replace whole indentation with sane one
            size_t size;
            char *buf = alloc_indent(info.level + count, &size);
            buffer_replace_bytes(info.bytes, buf, size);
            free(buf);
        }
        if (++i == nr_lines) {
            break;
        }
        block_iter_eat_line(&view->cursor);
    }
    free(indent);
}

static void shift_left(size_t nr_lines, size_t count)
{
    size_t i = 0;
    while (1) {
        IndentInfo info;
        LineRef lr;

        fetch_this_line(&view->cursor, &lr);
        get_indent_info(lr.line, lr.size, &info);
        if (info.wsonly) {
            if (info.bytes) {
                // Remove indentation
                buffer_delete_bytes(info.bytes);
            }
        } else if (info.level && info.sane) {
            size_t n = count;
            if (n > info.level) {
                n = info.level;
            }
            if (use_spaces_for_indent()) {
                n *= buffer->options.indent_width;
            }
            buffer_delete_bytes(n);
        } else if (info.bytes) {
            // Replace whole indentation with sane one
            if (info.level > count) {
                size_t size;
                char *buf = alloc_indent(info.level - count, &size);
                buffer_replace_bytes(info.bytes, buf, size);
                free(buf);
            } else {
                buffer_delete_bytes(info.bytes);
            }
        }
        if (++i == nr_lines) {
            break;
        }
        block_iter_eat_line(&view->cursor);
    }
}

static void do_shift_lines(int count, size_t nr_lines)
{
    begin_change_chain();
    block_iter_bol(&view->cursor);
    if (count > 0) {
        shift_right(nr_lines, count);
    } else {
        shift_left(nr_lines, -count);
    }
    end_change_chain();
}

void shift_lines(int count)
{
    long x = view_get_preferred_x(view) + buffer->options.indent_width * count;
    if (x < 0) {
        x = 0;
    }

    if (view->selection) {
        SelectionInfo info;
        view->selection = SELECT_LINES;
        init_selection(view, &info);
        view->cursor = info.si;
        size_t nr_lines = get_nr_selected_lines(&info);
        do_shift_lines(count, nr_lines);
        if (info.swapped) {
            // Cursor should be at beginning of selection
            block_iter_bol(&view->cursor);
            view->sel_so = block_iter_get_offset(&view->cursor);
            while (--nr_lines) {
                block_iter_prev_line(&view->cursor);
            }
        } else {
            BlockIter save = view->cursor;
            while (--nr_lines) {
                block_iter_prev_line(&view->cursor);
            }
            view->sel_so = block_iter_get_offset(&view->cursor);
            view->cursor = save;
        }
    } else {
        do_shift_lines(count, 1);
    }
    move_to_preferred_x(x);
}

void clear_lines(void)
{
    size_t del_count = 0, ins_count = 0;
    char *indent = NULL;

    if (buffer->options.auto_indent) {
        BlockIter bi = view->cursor;

        if (block_iter_prev_line(&bi) && find_non_empty_line_bwd(&bi)) {
            LineRef lr;
            fill_line_ref(&bi, &lr);
            indent = get_indent_for_next_line(lr.line, lr.size);
        }
    }

    if (view->selection) {
        view->selection = SELECT_LINES;
        del_count = prepare_selection(view);
        unselect();

        // Don't delete last newline
        if (del_count) {
            del_count--;
        }
    } else {
        block_iter_eol(&view->cursor);
        del_count = block_iter_bol(&view->cursor);
    }

    if (!indent && !del_count) {
        return;
    }

    if (indent) {
        ins_count = strlen(indent);
    }
    buffer_replace_bytes(del_count, indent, ins_count);
    free(indent);
    block_iter_skip_bytes(&view->cursor, ins_count);
}

void new_line(void)
{
    size_t ins_count = 1;
    char *ins = NULL;

    block_iter_eol(&view->cursor);

    if (buffer->options.auto_indent) {
        BlockIter bi = view->cursor;

        if (find_non_empty_line_bwd(&bi)) {
            LineRef lr;
            fill_line_ref(&bi, &lr);
            ins = get_indent_for_next_line(lr.line, lr.size);
        }
    }

    if (ins) {
        ins_count = strlen(ins);
        memmove(ins + 1, ins, ins_count);
        ins[0] = '\n';
        ins_count++;
        buffer_insert_bytes(ins, ins_count);
        free(ins);
    } else {
        buffer_insert_bytes("\n", 1);
    }

    block_iter_skip_bytes(&view->cursor, ins_count);
}

static void add_word(ParagraphFormatter *pf, const char *word, size_t len)
{
    size_t i = 0;
    size_t word_width = 0;

    while (i < len) {
        word_width += u_char_width(u_get_char(word, len, &i));
    }

    if (pf->cur_width && pf->cur_width + 1 + word_width > pf->text_width) {
        string_add_byte(&pf->buf, '\n');
        pf->cur_width = 0;
    }

    if (pf->cur_width == 0) {
        if (pf->indent_len) {
            string_add_buf(&pf->buf, pf->indent, pf->indent_len);
        }
        pf->cur_width = pf->indent_width;
    } else {
        string_add_byte(&pf->buf, ' ');
        pf->cur_width++;
    }

    string_add_buf(&pf->buf, word, len);
    pf->cur_width += word_width;
}

static bool is_paragraph_separator(const char *line, size_t size)
{
    return regexp_match_nosub("^[ \t]*(/\\*|\\*/)?[ \t]*$", line, size);
}

static size_t get_indent_width(const char *line, size_t size)
{
    IndentInfo info;
    get_indent_info(line, size, &info);
    return info.width;
}

static bool in_paragraph(const char *line, size_t size, size_t indent_width)
{
    if (get_indent_width(line, size) != indent_width) {
        return false;
    }
    return !is_paragraph_separator(line, size);
}

static size_t paragraph_size(void)
{
    BlockIter bi = view->cursor;
    LineRef lr;

    block_iter_bol(&bi);
    fill_line_ref(&bi, &lr);
    if (is_paragraph_separator(lr.line, lr.size)) {
        // Not in paragraph
        return 0;
    }
    size_t indent_width = get_indent_width(lr.line, lr.size);

    // Go to beginning of paragraph
    while (block_iter_prev_line(&bi)) {
        fill_line_ref(&bi, &lr);
        if (!in_paragraph(lr.line, lr.size, indent_width)) {
            block_iter_eat_line(&bi);
            break;
        }
    }
    view->cursor = bi;

    // Get size of paragraph
    size_t size = 0;
    do {
        size_t bytes = block_iter_eat_line(&bi);

        if (!bytes) {
            break;
        }

        size += bytes;
        fill_line_ref(&bi, &lr);
    } while (in_paragraph(lr.line, lr.size, indent_width));
    return size;
}

void format_paragraph(size_t text_width)
{
    size_t len;
    if (view->selection) {
        view->selection = SELECT_LINES;
        len = prepare_selection(view);
    } else {
        len = paragraph_size();
    }
    if (!len) {
        return;
    }

    char *sel = block_iter_get_bytes(&view->cursor, len);
    size_t indent_width = get_indent_width(sel, len);
    char *indent = make_indent(indent_width);

    ParagraphFormatter pf = {
        .buf = STRING_INIT,
        .indent = indent,
        .indent_len = indent ? strlen(indent) : 0,
        .indent_width = indent_width,
        .cur_width = 0,
        .text_width = text_width
    };

    size_t i = 0;
    while (1) {
        size_t start, tmp;

        while (i < len) {
            tmp = i;
            if (!u_is_breakable_whitespace(u_get_char(sel, len, &tmp))) {
                break;
            }
            i = tmp;
        }
        if (i == len) {
            break;
        }

        start = i;
        while (i < len) {
            tmp = i;
            if (u_is_breakable_whitespace(u_get_char(sel, len, &tmp))) {
                break;
            }
            i = tmp;
        }

        add_word(&pf, sel + start, i - start);
    }

    if (pf.buf.len) {
        string_add_byte(&pf.buf, '\n');
    }
    buffer_replace_bytes(len, pf.buf.buffer, pf.buf.len);
    if (pf.buf.len) {
        block_iter_skip_bytes(&view->cursor, pf.buf.len - 1);
    }
    string_free(&pf.buf);
    free(pf.indent);
    free(sel);

    unselect();
}

void change_case(int mode)
{
    bool was_selecting = false;
    bool move = true;
    size_t text_len, i;
    char *src;
    String dst = STRING_INIT;

    if (view->selection) {
        SelectionInfo info;
        init_selection(view, &info);
        view->cursor = info.si;
        text_len = info.eo - info.so;
        unselect();
        was_selecting = true;
        move = !info.swapped;
    } else {
        CodePoint u;
        if (!block_iter_get_char(&view->cursor, &u)) {
            return;
        }
        text_len = u_char_size(u);
    }

    src = block_iter_get_bytes(&view->cursor, text_len);
    i = 0;
    while (i < text_len) {
        CodePoint u = u_get_char(src, text_len, &i);

        switch (mode) {
        case 't':
            if (iswupper(u)) {
                u = towlower(u);
            } else {
                u = towupper(u);
            }
            break;
        case 'l':
            u = towlower(u);
            break;
        case 'u':
            u = towupper(u);
            break;
        }
        string_add_ch(&dst, u);
    }

    buffer_replace_bytes(text_len, dst.buffer, dst.len);
    free(src);

    if (move && dst.len > 0) {
        if (was_selecting) {
            // Move cursor back to where it was
            size_t idx = dst.len;
            u_prev_char(dst.buffer, &idx);
            block_iter_skip_bytes(&view->cursor, idx);
        } else {
            block_iter_skip_bytes(&view->cursor, dst.len);
        }
    }

    string_free(&dst);
}
