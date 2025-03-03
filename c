use crossterm::{
    event::{poll, read, Event, KeyCode, KeyEvent, KeyModifiers},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use copypasta::{ClipboardContext, ClipboardProvider};
use std::{
    convert::TryInto,
    io::{self, Write},
    path::PathBuf,
    time::Duration,
};
use tui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Span, Spans, Text},
    widgets::{Paragraph, Wrap, Block, Borders},
    Frame, Terminal,
};
use unicode_segmentation::UnicodeSegmentation;
use unicode_width::UnicodeWidthStr;

#[derive(Clone)]
enum Mode {
    Editor,
    FileTree,
}

struct FileTree {
    current_path: PathBuf,
    entries: Vec<std::fs::DirEntry>,
    selected: usize,
    scroll_offset: usize,
}

impl FileTree {
    fn new() -> Self {
        let current_path = std::env::current_dir().unwrap();
        let mut ft = FileTree {
            current_path,
            entries: Vec::new(),
            selected: 0,
            scroll_offset: 0,
        };
        ft.refresh();
        ft
    }
    fn refresh(&mut self) {
        self.entries = std::fs::read_dir(&self.current_path)
            .unwrap()
            .filter_map(|e| e.ok())
            .collect();
        self.entries.sort_by_key(|e| e.path());
        self.selected = 0;
        self.scroll_offset = 0;
    }
    fn move_up(&mut self) {
        if self.selected > 0 { self.selected -= 1; }
    }
    fn move_down(&mut self) {
        if self.selected + 1 < self.entries.len() { self.selected += 1; }
    }
    fn enter(&mut self) {
        if self.entries.is_empty() { return; }
        let entry = &self.entries[self.selected];
        let path = entry.path();
        if path.is_dir() {
            self.current_path = path;
            self.refresh();
        }
    }
    fn go_up(&mut self) {
        if let Some(parent) = self.current_path.parent() {
            self.current_path = parent.to_path_buf();
            self.refresh();
        }
    }
    fn update_scroll(&mut self, visible: usize) {
        if self.selected < self.scroll_offset {
            self.scroll_offset = self.selected;
        } else if self.selected >= self.scroll_offset + visible {
            self.scroll_offset = self.selected.saturating_sub(visible - 1);
        }
    }
}

impl Clone for FileTree {
    fn clone(&self) -> Self {
        let mut ft = FileTree::new();
        ft.current_path = self.current_path.clone();
        ft.refresh();
        ft.selected = self.selected;
        ft.scroll_offset = self.scroll_offset;
        ft
    }
}

struct App {
    mode: Mode,
    // Editor state
    lines: Vec<String>,
    cursor_x: usize,
    cursor_y: usize,
    scroll_offset: usize,
    h_scroll_offset: usize, // 横スクロール用
    shift_selection: bool,
    sel_start: Option<(usize, usize)>,
    sel_end: Option<(usize, usize)>,
    current_file: Option<PathBuf>,
    // Clipboard (system)
    clipboard_ctx: Option<ClipboardContext>,
    // Undo/Redo
    undo_stack: Vec<Vec<String>>,
    redo_stack: Vec<Vec<String>>,
    help_visible: bool,
    // FileTree state
    file_tree: FileTree,
    // ALT加速用
    alt_n: usize,
}

impl Clone for App {
    fn clone(&self) -> Self {
        App {
            mode: self.mode.clone(),
            lines: self.lines.clone(),
            cursor_x: self.cursor_x,
            cursor_y: self.cursor_y,
            scroll_offset: self.scroll_offset,
            h_scroll_offset: self.h_scroll_offset,
            shift_selection: self.shift_selection,
            sel_start: self.sel_start,
            sel_end: self.sel_end,
            current_file: self.current_file.clone(),
            clipboard_ctx: None,
            undo_stack: self.undo_stack.clone(),
            redo_stack: self.redo_stack.clone(),
            help_visible: self.help_visible,
            file_tree: self.file_tree.clone(),
            alt_n: self.alt_n,
        }
    }
}

impl App {
    fn new() -> Self {
        App {
            mode: Mode::Editor,
            lines: vec![String::new()],
            cursor_x: 0,
            cursor_y: 0,
            scroll_offset: 0,
            h_scroll_offset: 0,
            shift_selection: false,
            sel_start: None,
            sel_end: None,
            current_file: None,
            clipboard_ctx: ClipboardContext::new().ok(),
            undo_stack: Vec::new(),
            redo_stack: Vec::new(),
            help_visible: false,
            file_tree: FileTree::new(),
            alt_n: 8,
        }
    }

    // --- Editor operations ---
    fn insert_char(&mut self, c: char) {
        if self.sel_start.is_some() && self.sel_end.is_some() && self.sel_start != self.sel_end {
            self.delete_selection();
        }
        self.save_undo();
        let line_len = self.lines[self.cursor_y].len();
        if self.cursor_x > line_len { self.cursor_x = line_len; }
        self.lines[self.cursor_y].insert(self.cursor_x, c);
        self.cursor_x += 1;
        self.adjust_h_scroll(0);
    }
    fn insert_newline(&mut self) {
        if self.sel_start.is_some() && self.sel_end.is_some() && self.sel_start != self.sel_end {
            self.delete_selection();
        }
        self.save_undo();
        let line_len = self.lines[self.cursor_y].len();
        if self.cursor_x > line_len { self.cursor_x = line_len; }
        let tail = self.lines[self.cursor_y].split_off(self.cursor_x);
        self.cursor_y += 1;
        self.lines.insert(self.cursor_y, tail);
        self.cursor_x = 0;
        self.adjust_h_scroll(0);
    }
    fn backspace(&mut self) {
        if self.sel_start.is_some() && self.sel_end.is_some() && self.sel_start != self.sel_end {
            self.delete_selection();
            return;
        }
        if self.cursor_x == 0 && self.cursor_y == 0 { return; }
        self.save_undo();
        if self.cursor_x > 0 {
            self.cursor_x -= 1;
            self.lines[self.cursor_y].remove(self.cursor_x);
        } else if self.cursor_y > 0 {
            let current_line = self.lines.remove(self.cursor_y);
            self.cursor_y -= 1;
            let old_len = self.lines[self.cursor_y].len();
            self.lines[self.cursor_y].push_str(&current_line);
            self.cursor_x = old_len;
        }
        self.adjust_h_scroll(0);
    }
    fn delete_selection(&mut self) {
        if let (Some((sy, sx)), Some((ey, ex))) = (self.sel_start, self.sel_end) {
            let ((start_y, start_x), (end_y, end_x)) = if (sy, sx) <= (ey, ex) {
                ((sy, sx), (ey, ex))
            } else {
                ((ey, ex), (sy, sx))
            };
            self.save_undo();
            if start_y == end_y {
                self.lines[start_y].replace_range(start_x..end_x, "");
                self.cursor_y = start_y;
                self.cursor_x = start_x;
            } else {
                let first_part = self.lines[start_y][..start_x].to_string();
                let last_part = self.lines[end_y][end_x.min(self.lines[end_y].len())..].to_string();
                self.lines[start_y] = first_part + &last_part;
                for _ in start_y+1..=end_y { self.lines.remove(start_y+1); }
                self.cursor_y = start_y;
                self.cursor_x = start_x;
            }
            self.selection_reset();
            self.adjust_h_scroll(0);
        }
    }
    fn update_selection(&mut self, old: (usize, usize)) {
        if self.sel_start.is_none() { self.sel_start = Some(old); }
        self.sel_end = Some((self.cursor_y, self.cursor_x));
    }
    fn selection_reset(&mut self) {
        self.sel_start = None;
        self.sel_end = None;
    }
    fn select_all(&mut self) {
        self.sel_start = Some((0, 0));
        let last_line = self.lines.len().saturating_sub(1);
        let end_x = self.lines[last_line].len();
        self.sel_end = Some((last_line, end_x));
        self.shift_selection = true;
    }

    // --- Clipboard operations ---
    fn copy_selection(&mut self) { if let Some(text) = self.get_selected_text() {
        if let Some(ctx) = self.clipboard_ctx.as_mut() { let _ = ctx.set_contents(text); }
    } }
    fn cut_selection(&mut self) { self.copy_selection(); self.delete_selection(); }
    fn paste_clipboard(&mut self) {
        if let Some(ctx) = self.clipboard_ctx.as_mut() {
            if let Ok(contents) = ctx.get_contents() {
                self.save_undo();
                let mut lines_iter = contents.split('\n').peekable();
                while let Some(text_part) = lines_iter.next() {
                    let line_len = self.lines[self.cursor_y].len();
                    if self.cursor_x > line_len { self.cursor_x = line_len; }
                    self.lines[self.cursor_y].insert_str(self.cursor_x, text_part);
                    self.cursor_x += text_part.len();
                    if lines_iter.peek().is_some() { self.insert_newline(); }
                }
                self.adjust_h_scroll(0);
            }
        }
    }
    fn get_selected_text(&self) -> Option<String> {
        let (sy, sx) = self.sel_start?;
        let (ey, ex) = self.sel_end?;
        let ((start_y, start_x), (end_y, end_x)) = if (sy, sx) <= (ey, ex) {
            ((sy, sx), (ey, ex))
        } else { ((ey, ex), (sy, sx)) };
        let mut result = String::new();
        for row in start_y..=end_y {
            let line = &self.lines[row];
            if start_y == end_y {
                result.push_str(&line[start_x.min(line.len())..end_x.min(line.len())]);
            } else if row == start_y {
                result.push_str(&line[start_x.min(line.len())..]);
                result.push('\n');
            } else if row == end_y {
                result.push_str(&line[..end_x.min(line.len())]);
            } else {
                result.push_str(line);
                result.push('\n');
            }
        }
        Some(result)
    }

    // --- Undo/Redo ---
    fn save_undo(&mut self) { self.undo_stack.push(self.lines.clone()); self.redo_stack.clear(); }
    fn undo(&mut self) {
        if let Some(prev) = self.undo_stack.pop() {
            self.redo_stack.push(self.lines.clone());
            self.lines = prev;
            self.cursor_y = self.cursor_y.min(self.lines.len().saturating_sub(1));
            self.cursor_x = self.cursor_x.min(self.lines[self.cursor_y].len());
            self.adjust_h_scroll(0);
        }
    }
    fn redo(&mut self) {
        if let Some(next) = self.redo_stack.pop() {
            self.undo_stack.push(self.lines.clone());
            self.lines = next;
            self.cursor_y = self.cursor_y.min(self.lines.len().saturating_sub(1));
            self.cursor_x = self.cursor_x.min(self.lines[self.cursor_y].len());
            self.adjust_h_scroll(0);
        }
    }

    // --- Horizontal scroll (Editor) ---
    fn adjust_h_scroll(&mut self, available_width: usize) {
        let avail = if available_width == 0 { 80 } else { available_width };
        let line = &self.lines[self.cursor_y];
        let graphemes: Vec<&str> = line.graphemes(true).collect();
        let current_width: usize = graphemes[..self.cursor_x.min(graphemes.len())]
            .iter().map(|g| g.width()).sum();
        if current_width < self.h_scroll_offset {
            self.h_scroll_offset = current_width;
        } else if current_width >= self.h_scroll_offset + avail {
            self.h_scroll_offset = current_width.saturating_sub(avail) + 1;
        }
    }

    // --- Cursor movement (Editor) ---
    fn handle_arrow_key(&mut self, code: KeyCode) {
        let old = (self.cursor_y, self.cursor_x);
        match code {
            KeyCode::Left => self.move_left(),
            KeyCode::Right => self.move_right(),
            KeyCode::Up => self.move_up(),
            KeyCode::Down => self.move_down(),
            _ => {}
        }
        if self.shift_selection {
            if self.sel_start.is_none() { self.sel_start = Some(old); }
            self.sel_end = Some((self.cursor_y, self.cursor_x));
        }
        self.adjust_h_scroll(0);
    }
    fn move_left(&mut self) {
        if self.cursor_x > 0 { self.cursor_x -= 1; }
        else if self.cursor_y > 0 {
            self.cursor_y -= 1;
            self.cursor_x = self.lines[self.cursor_y].len();
        }
    }
    fn move_right(&mut self) {
        let line_len = self.lines[self.cursor_y].len();
        if self.cursor_x < line_len { self.cursor_x += 1; }
        else if self.cursor_y + 1 < self.lines.len() {
            self.cursor_y += 1;
            self.cursor_x = 0;
        }
    }
    fn move_up(&mut self) {
        if self.cursor_y > 0 {
            self.cursor_y -= 1;
            let line_len = self.lines[self.cursor_y].len();
            self.cursor_x = self.cursor_x.min(line_len);
        }
    }
    fn move_down(&mut self) {
        if self.cursor_y + 1 < self.lines.len() {
            self.cursor_y += 1;
            let line_len = self.lines[self.cursor_y].len();
            self.cursor_x = self.cursor_x.min(line_len);
        }
    }
    fn move_word_left(&mut self) {
        if self.cursor_x == 0 && self.cursor_y == 0 { return; }
        if self.cursor_x == 0 {
            self.cursor_y -= 1;
            self.cursor_x = self.lines[self.cursor_y].len();
            return;
        }
        let line = &self.lines[self.cursor_y];
        let mut idx = self.cursor_x;
        let graphemes: Vec<&str> = line.graphemes(true).collect();
        while idx > 0 {
            idx -= 1;
            if graphemes[idx] == " " || graphemes[idx] == "\t" { break; }
        }
        self.cursor_x = idx;
    }
    fn move_word_right(&mut self) {
        let line_len = self.lines[self.cursor_y].len();
        if self.cursor_y == self.lines.len()-1 && self.cursor_x == line_len { return; }
        if self.cursor_x == line_len {
            self.cursor_y += 1;
            self.cursor_x = 0;
            return;
        }
        let line = &self.lines[self.cursor_y];
        let graphemes: Vec<&str> = line.graphemes(true).collect();
        let mut idx = self.cursor_x;
        while idx < graphemes.len() {
            idx += 1;
            if idx >= graphemes.len() { break; }
            if graphemes[idx] == " " || graphemes[idx] == "\t" {
                idx += 1;
                break;
            }
        }
        self.cursor_x = idx.min(line_len);
    }
    fn move_alt_left(&mut self) {
        for _ in 0..self.alt_n { self.move_left(); }
        self.alt_n = (self.alt_n * 2).min(128);
    }
    fn move_alt_right(&mut self) {
        for _ in 0..self.alt_n { self.move_right(); }
        self.alt_n = (self.alt_n * 2).min(128);
    }

    // --- Scrolling ---
    fn scroll_up(&mut self) { if self.scroll_offset > 0 { self.scroll_offset -= 1; } }
    fn scroll_down(&mut self) { if self.scroll_offset < self.lines.len().saturating_sub(1) { self.scroll_offset += 1; } }
    fn adjust_scroll(&mut self, visible_height: usize) {
        if self.cursor_y < self.scroll_offset {
            self.scroll_offset = self.cursor_y;
        } else if self.cursor_y >= self.scroll_offset + visible_height {
            self.scroll_offset = self.cursor_y.saturating_sub(visible_height - 1);
        }
    }
    fn line_number_width(&self) -> usize {
        let total = self.lines.len();
        format!("{}", total).len().max(2)
    }

    // --- Search & Save ---
    fn search(&mut self) {
        let mut query = String::new();
        loop {
            if let Event::Key(KeyEvent { code, .. }) = read().unwrap() {
                match code {
                    KeyCode::Enter => break,
                    KeyCode::Esc => { query.clear(); break; },
                    KeyCode::Backspace => { query.pop(); },
                    KeyCode::Char(c) => { query.push(c); },
                    _ => {}
                }
            }
        }
        if query.is_empty() { return; }
        let mut found = false;
        for (i, line) in self.lines.iter().enumerate().skip(self.cursor_y) {
            if let Some(pos) = line.find(&query) {
                self.cursor_y = i;
                self.cursor_x = pos;
                found = true;
                break;
            }
        }
        if !found {
            for (i, line) in self.lines.iter().enumerate().take(self.cursor_y) {
                if let Some(pos) = line.find(&query) {
                    self.cursor_y = i;
                    self.cursor_x = pos;
                    break;
                }
            }
        }
        self.adjust_h_scroll(0);
    }
    fn save_file(&mut self) {
        let content = self.lines.join("\n");
        if let Some(ref path) = self.current_file {
            let _ = std::fs::write(path, content);
        } else {
            disable_raw_mode().unwrap();
            print!("Enter file name to save: ");
            io::stdout().flush().unwrap();
            let mut filename = String::new();
            io::stdin().read_line(&mut filename).unwrap();
            let filename = filename.trim();
            enable_raw_mode().unwrap();
            if !filename.is_empty() {
                self.current_file = Some(PathBuf::from(filename));
                let _ = std::fs::write(filename, content);
            }
        }
    }
    fn exit_prompt(&mut self) -> Option<String> {
        // ポップアップ風に中央に表示
        disable_raw_mode().unwrap();
        println!("\n==================================");
        println!("  Exit options: [e]xit, [s]ave and exit, [c]ancel");
        println!("==================================");
        let mut input = String::new();
        io::stdin().read_line(&mut input).unwrap();
        enable_raw_mode().unwrap();
        let choice = input.trim().to_lowercase();
        if choice == "e" { Some("exit".to_string()) }
        else if choice == "s" { Some("save".to_string()) }
        else { None }
    }

    // --- FileTree mode operations ---
    fn file_tree_move_up(&mut self) { self.file_tree.move_up(); }
    fn file_tree_move_down(&mut self) { self.file_tree.move_down(); }
    fn file_tree_enter(&mut self) {
        if self.file_tree.entries.is_empty() { return; }
        let entry = &self.file_tree.entries[self.file_tree.selected];
        let path = entry.path();
        if path.is_dir() {
            self.file_tree.enter();
        } else {
            if let Ok(content) = std::fs::read_to_string(&path) {
                self.lines = content.lines().map(|s| s.to_string()).collect();
                if self.lines.is_empty() { self.lines.push(String::new()); }
                self.cursor_x = 0;
                self.cursor_y = 0;
                self.current_file = Some(path);
                self.mode = Mode::Editor;
            }
        }
    }
    fn file_tree_go_up(&mut self) { self.file_tree.go_up(); }

    // --- New file popup (n) ---
    fn new_file_popup(&mut self) {
        disable_raw_mode().unwrap();
        println!("\n--- New File ---");
        print!("Enter new file path (can include directories): ");
        io::stdout().flush().unwrap();
        let mut filename = String::new();
        io::stdin().read_line(&mut filename).unwrap();
        let filename = filename.trim();
        enable_raw_mode().unwrap();
        if !filename.is_empty() {
            // Create file (and parent directories if needed)
            let path = PathBuf::from(filename);
            if let Some(parent) = path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let _ = std::fs::write(&path, "");
            self.current_file = Some(path);
            self.lines = vec![String::new()];
            self.cursor_x = 0;
            self.cursor_y = 0;
            self.mode = Mode::Editor;
        }
    }

    // --- Rename / Move popup (m) ---
    fn rename_file_popup(&mut self) {
        if let Some(ref path) = self.current_file {
            disable_raw_mode().unwrap();
            println!("\n--- Rename File ---");
            print!("Enter new name (can include directories): ");
            io::stdout().flush().unwrap();
            let mut newname = String::new();
            io::stdin().read_line(&mut newname).unwrap();
            let newname = newname.trim();
            enable_raw_mode().unwrap();
            if !newname.is_empty() {
                let new_path = PathBuf::from(newname);
                if std::fs::rename(path, &new_path).is_ok() {
                    self.current_file = Some(new_path);
                }
            }
        }
    }

    // --- Delete popup (Del key) ---
    fn delete_file_popup(&mut self) {
        if let Some(ref path) = self.current_file {
            let metadata = std::fs::metadata(path).ok();
            let large = metadata.map(|m| m.len() > 1_000_000).unwrap_or(false);
            disable_raw_mode().unwrap();
            println!("\n--- Delete File ---");
            if large {
                print!("This file is large. To confirm deletion, re-enter file name: ");
            } else {
                print!("Confirm deletion? (y/n): ");
            }
            io::stdout().flush().unwrap();
            let mut input = String::new();
            io::stdin().read_line(&mut input).unwrap();
            let input = input.trim();
            enable_raw_mode().unwrap();
            let confirm = if large {
                // For large files, the file name must match exactly.
                if let Some(ref p) = self.current_file {
                    p.file_name().and_then(|s| s.to_str()).unwrap_or("") == input
                } else {
                    false
                }
            } else {
                input == "y" || input == "Y"
            };
            if confirm {
                let _ = std::fs::remove_file(path);
                self.current_file = None;
                self.lines = vec![String::new()];
                self.cursor_x = 0;
                self.cursor_y = 0;
            }
        }
    }
}

// --- Drawing functions ---

fn draw_header<B: tui::backend::Backend>(frame: &mut Frame<B>, app: &App, area: Rect) {
    let header_text = if let Some(ref path) = app.current_file {
        let file_name = path.file_name().and_then(|s| s.to_str()).unwrap_or("Unknown");
        let full_path = path.to_string_lossy();
        let truncated = if full_path.len() > 30 { format!("{}...", &full_path[..30]) } else { full_path.to_string() };
        format!("File: {} | {}", file_name, truncated)
    } else {
        "New File".to_string()
    };
    let paragraph = Paragraph::new(header_text)
        .style(Style::default().fg(Color::Rgb(222, 165, 132)).bg(Color::Rgb(33, 40, 48)));
    frame.render_widget(paragraph, area);
}

fn draw_editor<B: tui::backend::Backend>(
    frame: &mut Frame<B>,
    app: &mut App,
    chunks: [Rect; 3],
    update_state: bool,
) {
    let editor_height = chunks[1].height as usize;
    if update_state {
        app.adjust_scroll(editor_height);
        app.adjust_h_scroll(chunks[1].width as usize);
    }
    let start = app.scroll_offset;
    let end = (start + editor_height).min(app.lines.len());
    let display_lines = &app.lines[start..end];

    // --- 行番号欄 ---
    let mut line_no_spans = Vec::new();
    let digits = app.line_number_width();
    for (i, _) in display_lines.iter().enumerate() {
        let real_line = start + i;
        let lineno_text = format!("{:>width$}", real_line + 1, width = digits);
        if real_line == app.cursor_y {
            line_no_spans.push(Spans::from(Span::styled(
                lineno_text,
                Style::default().bg(Color::White).fg(Color::Black),
            )));
        } else {
            line_no_spans.push(Spans::from(Span::raw(lineno_text)));
        }
    }
    let paragraph_line_no = Paragraph::new(line_no_spans).wrap(Wrap { trim: false });
    frame.render_widget(paragraph_line_no, chunks[0]);

    // --- テキスト欄 (横スクロール対応) ---
    let available_width = chunks[1].width as usize;
    let mut text_spans = Vec::new();
    let selection = match (app.sel_start, app.sel_end) {
        (Some(s), Some(e)) => Some(if s <= e { (s, e) } else { (e, s) }),
        _ => None,
    };
    for (i, line) in display_lines.iter().enumerate() {
        let real_line = start + i;
        let graphemes: Vec<&str> = line.graphemes(true).collect();
        // 横スクロールの開始位置を計算
        let mut cum = 0;
        let mut start_idx = 0;
        for (j, g) in graphemes.iter().enumerate() {
            cum += g.width();
            if cum > app.h_scroll_offset {
                start_idx = j;
                break;
            }
        }
        let mut displayed = String::new();
        let mut width = 0;
        for g in graphemes.iter().skip(start_idx) {
            let w = g.width();
            if width + w > available_width { break; }
            displayed.push_str(g);
            width += w;
        }
        // 選択状態は、ここでは全体を簡易的にハイライト
        let span = if let Some(((sy, _), (ey, _))) = selection {
            if real_line >= sy && real_line <= ey {
                Spans::from(Span::styled(displayed, Style::default().bg(Color::LightBlue)))
            } else {
                Spans::from(Span::raw(displayed))
            }
        } else {
            Spans::from(Span::raw(displayed))
        };
        text_spans.push(span);
    }
    let paragraph_text = Paragraph::new(text_spans).wrap(Wrap { trim: false });
    frame.render_widget(paragraph_text, chunks[1]);

    // --- スクロールバー (Editor) ---
    let total_lines = app.lines.len();
    let mut scrollbar_spans = Vec::new();
    if total_lines <= editor_height {
        for _ in 0..editor_height { scrollbar_spans.push(Spans::from(" ")); }
    } else {
        let max_scroll = total_lines.saturating_sub(editor_height);
        let ratio = app.scroll_offset as f32 / max_scroll as f32;
        let thumb_row = (ratio * (editor_height - 1) as f32).round() as usize;
        for row in 0..editor_height {
            if row == thumb_row { scrollbar_spans.push(Spans::from("█")); }
            else { scrollbar_spans.push(Spans::from(" ")); }
        }
    }
    let paragraph_scrollbar = Paragraph::new(scrollbar_spans).wrap(Wrap { trim: false });
    frame.render_widget(paragraph_scrollbar, chunks[2]);

    // --- カーソル位置 (横スクロール対応) ---
    if app.cursor_y >= start && app.cursor_y < end {
        let row_in_view = app.cursor_y - start;
        let line = &app.lines[app.cursor_y];
        let graphemes: Vec<&str> = line.graphemes(true).collect();
        let mut cum = 0;
        for (j, g) in graphemes.iter().enumerate() {
            cum += g.width();
            if j == app.cursor_x { break; }
        }
        // 誤差対策：カーソルは実際の幅分をそのまま描画（要求：中間ならずれるのを修正）
        let cursor_screen_x = if cum > app.h_scroll_offset { cum - app.h_scroll_offset } else { 0 } as u16;
        let cursor_x = chunks[1].x + cursor_screen_x;
        let cursor_y = chunks[1].y + row_in_view as u16;
        // 修正：カーソルが中間文字の場合はオフセットを1減らす
        let final_cursor_x = if app.cursor_x < graphemes.len() && cum > 0 {
            cursor_x.saturating_sub(1)
        } else { cursor_x };
        frame.set_cursor(final_cursor_x, cursor_y);
    } else {
        frame.set_cursor(0, 0);
    }
}

fn draw_status_bar<B: tui::backend::Backend>(frame: &mut Frame<B>, app: &App, area: Rect) {
    let total_lines = app.lines.len();
    let (cur_line, cur_col) = (app.cursor_y + 1, app.cursor_x + 1);
    let mode_text = match app.mode { Mode::Editor => "Editor", Mode::FileTree => "FileTree", };
    let status_text = format!(
        "[RWE] {} | lines: {}  Ln {}, Col {}  (Ctrl+S=Save, Esc=Exit, F11=Help, F2=FileTree, F1=Editor)",
        mode_text, total_lines, cur_line, cur_col
    );
    let style = match app.mode {
        Mode::FileTree => Style::default().bg(Color::Rgb(33, 40, 48)).fg(Color::LightBlue),
        _ => Style::default(),
    };
    let paragraph = Paragraph::new(status_text).style(style);
    frame.render_widget(paragraph, area);
}

fn draw_help_screen<B: tui::backend::Backend>(frame: &mut Frame<B>, app: &App) {
    let size = frame.size();
    let mut help_text = Text::raw(
r#"=== Key Bindings Help ===

-- General --
F11 ........ Toggle Help
Esc ........ Exit prompt (e: exit, s: save and exit, c: cancel)

-- Editor Mode --
Arrow keys .......... Move cursor
Shift + Arrow ....... Select region (highlighted in LightBlue)
Ctrl + Left/Right ... Move by word
Alt + Left/Right .... Jump with acceleration (2^n)
Ctrl + c ............ Copy
Ctrl + x ............ Cut
Ctrl + v ............ Paste
Ctrl + a ............ Select all
Ctrl + z / r ........ Undo / Redo
Ctrl + Up/Down ...... Scroll view
Ctrl + f ............ Search text
Ctrl + S ............ Save file
n ................... New file
m ................... Rename/Move file
Del ............... Delete file

-- FileTree Mode --
F2 ........ Switch to FileTree mode
(Press a number key to open that entry)
Up/Down .... Navigate entries
Right ..... Enter directory
Left ...... Go up a directory
Enter ..... Open file in Editor
F1 ........ Switch to Editor mode
"#
    );
    if app.shift_selection { help_text.extend(Text::raw("\n(Shift selection in progress)")); }
    let paragraph = Paragraph::new(help_text)
        .wrap(Wrap { trim: false })
        .style(Style::default().add_modifier(Modifier::BOLD));
    frame.render_widget(paragraph, size);
}

fn draw_file_tree<B: tui::backend::Backend>(frame: &mut Frame<B>, app: &App, area: Rect) {
    // FileTree領域を左右に分割：左はエントリリスト、右はスクロールバー
    let chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(95), Constraint::Percentage(5)].as_ref())
        .split(area);
    let ft = &app.file_tree;
    let visible = chunks[0].height as usize;
    let mut items = Vec::new();
    // ヘッダーに行番号付き
    let header = format!("1: Path: {}", ft.current_path.display());
    items.push(Spans::from(Span::styled(header, Style::default().fg(Color::Rgb(222, 165, 132)).bg(Color::Rgb(33, 40, 48)))));
    let mut ft_clone = ft.clone();
    ft_clone.update_scroll(visible - 1);
    for (i, entry) in ft_clone.entries.iter().enumerate().skip(ft_clone.scroll_offset).take(visible - 1) {
        let line_no = i + 1;
        let file_name = entry.file_name().into_string().unwrap_or_default();
        let text = format!("{}: {}", line_no, file_name);
        // 数字キーで選択できるように（表示時、先頭数字は強調）
        let style = if i == ft_clone.selected {
            Style::default().bg(Color::Gray).fg(Color::Black)
        } else {
            Style::default().fg(Color::White)
        };
        items.push(Spans::from(Span::styled(text, style)));
    }
    let paragraph = Paragraph::new(items)
        .wrap(Wrap { trim: false })
        .style(Style::default().bg(Color::Rgb(33, 40, 48)));
    frame.render_widget(paragraph, chunks[0]);

    // --- FileTreeスクロールバー ---
    let total_entries = ft_clone.entries.len();
    let mut sb_items = Vec::new();
    if total_entries <= visible - 1 {
        for _ in 0..(visible - 1) { sb_items.push(Spans::from(" ")); }
    } else {
        let max_scroll = total_entries.saturating_sub(visible - 1);
        let ratio = ft_clone.scroll_offset as f32 / max_scroll as f32;
        let thumb = (ratio * ((visible - 1) as f32 - 1.0)).round() as usize;
        for i in 0..(visible - 1) {
            if i == thumb { sb_items.push(Spans::from("█")); }
            else { sb_items.push(Spans::from(" ")); }
        }
    }
    let sb_paragraph = Paragraph::new(sb_items)
        .wrap(Wrap { trim: false })
        .style(Style::default().bg(Color::Rgb(33, 40, 48)).fg(Color::LightBlue));
    frame.render_widget(sb_paragraph, chunks[1]);
}

fn draw_file_tree_mode<B: tui::backend::Backend>(frame: &mut Frame<B>, app: &App) {
    let size = frame.size();
    // FileTree用の全体レイアウト：左右分割
    let chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(70), Constraint::Percentage(30)].as_ref())
        .split(size);
    // 左側：エディタプレビュー（状態更新なし）
    let vertical_chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(2), Constraint::Min(1), Constraint::Length(1)])
        .split(chunks[0]);
    draw_header(frame, app, vertical_chunks[0]);
    let editor_chunks_vec = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Length(app.line_number_width() as u16 + 1),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(vertical_chunks[1]);
    let editor_chunks: [Rect; 3] = editor_chunks_vec.try_into().unwrap();
    draw_editor(frame, &mut app.clone(), editor_chunks, false);
    draw_status_bar(frame, app, vertical_chunks[2]);
    // 右側：FileTree
    draw_file_tree(frame, app, chunks[1]);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;
    let mut app = App::new();

    'main_loop: loop {
        terminal.draw(|frame| {
            if app.help_visible {
                draw_help_screen(frame, &app);
            } else if let Mode::FileTree = app.mode {
                draw_file_tree_mode(frame, &app);
            } else {
                let size = frame.size();
                let vertical_chunks = Layout::default()
                    .direction(Direction::Vertical)
                    .constraints([Constraint::Length(1), Constraint::Min(1), Constraint::Length(1)])
                    .split(size);
                draw_header(frame, &app, vertical_chunks[0]);
                let editor_chunks_vec = Layout::default()
                    .direction(Direction::Horizontal)
                    .constraints([
                        Constraint::Length(app.line_number_width() as u16 + 1),
                        Constraint::Min(1),
                        Constraint::Length(1),
                    ])
                    .split(vertical_chunks[1]);
                let editor_chunks: [Rect; 3] = editor_chunks_vec.try_into().unwrap();
                draw_editor(frame, &mut app, editor_chunks, true);
                draw_status_bar(frame, &app, vertical_chunks[2]);
            }
        })?;

        if poll(Duration::from_millis(100))? {
            if let Event::Key(KeyEvent { code, modifiers, .. }) = read()? {
                // ESC：共通のポップアップで終了／保存／キャンセル
                if code == KeyCode::Esc && !modifiers.contains(KeyModifiers::CONTROL) {
                    if let Some(choice) = app.exit_prompt() {
                        if choice == "exit" { break 'main_loop; }
                        else if choice == "save" { app.save_file(); break 'main_loop; }
                    }
                    continue;
                }
                // F11: ヘルプ切替
                if code == KeyCode::F(11) {
                    app.help_visible = !app.help_visible;
                    continue;
                }
                // モード切替：F2でFileTree、F1でEditor
                if code == KeyCode::F(2) { app.mode = Mode::FileTree; continue; }
                if code == KeyCode::F(1) { app.mode = Mode::Editor; continue; }
                match app.mode {
                    Mode::Editor => {
                        // ALTキー加速リセット
                        if !modifiers.contains(KeyModifiers::ALT) { app.alt_n = 8; }
                        if code == KeyCode::Char('s') && modifiers == KeyModifiers::CONTROL {
                            app.save_file(); continue;
                        }
                        if code == KeyCode::Up && modifiers.contains(KeyModifiers::CONTROL) {
                            app.scroll_up(); continue;
                        }
                        if code == KeyCode::Down && modifiers.contains(KeyModifiers::CONTROL) {
                            app.scroll_down(); continue;
                        }
                        if code == KeyCode::Char('f') && modifiers == KeyModifiers::CONTROL {
                            app.search(); continue;
                        }
                        if code == KeyCode::Char('c') && modifiers == KeyModifiers::CONTROL {
                            app.copy_selection(); continue;
                        }
                        if code == KeyCode::Char('x') && modifiers == KeyModifiers::CONTROL {
                            app.cut_selection(); continue;
                        }
                        if code == KeyCode::Char('v') && modifiers == KeyModifiers::CONTROL {
                            app.paste_clipboard(); continue;
                        }
                        if code == KeyCode::Char('a') && modifiers == KeyModifiers::CONTROL {
                            app.select_all(); continue;
                        }
                        if code == KeyCode::Char('z') && modifiers == KeyModifiers::CONTROL {
                            app.undo(); continue;
                        }
                        if code == KeyCode::Char('r') && modifiers == KeyModifiers::CONTROL {
                            app.redo(); continue;
                        }
                        if code == KeyCode::Left && modifiers == KeyModifiers::CONTROL {
                            app.move_word_left(); continue;
                        }
                        if code == KeyCode::Right && modifiers == KeyModifiers::CONTROL {
                            app.move_word_right(); continue;
                        }
                        if code == KeyCode::Left && modifiers == KeyModifiers::ALT {
                            app.move_alt_left(); continue;
                        }
                        if code == KeyCode::Right && modifiers == KeyModifiers::ALT {
                            app.move_alt_right(); continue;
                        }
                        if (code == KeyCode::Left || code == KeyCode::Right || code == KeyCode::Up || code == KeyCode::Down)
                            && modifiers.contains(KeyModifiers::SHIFT)
                        {
                            app.shift_selection = true;
                            app.handle_arrow_key(code);
                            continue;
                        }
                        if code == KeyCode::Left || code == KeyCode::Right || code == KeyCode::Up || code == KeyCode::Down {
                            app.shift_selection = false;
                            app.selection_reset();
                            app.handle_arrow_key(code);
                            continue;
                        }
                        // Editor専用：新規作成(n), リネーム(m), 削除(Del)
                        if code == KeyCode::Char('n') {
                            app.new_file_popup();
                            continue;
                        }
                        if code == KeyCode::Char('m') {
                            app.rename_file_popup();
                            continue;
                        }
                        if code == KeyCode::Delete {
                            app.delete_file_popup();
                            continue;
                        }
                        match code {
                            KeyCode::Char(c) => {
                                app.insert_char(c);
                                if !modifiers.contains(KeyModifiers::SHIFT) {
                                    app.shift_selection = false;
                                    app.selection_reset();
                                }
                            }
                            KeyCode::Enter => {
                                app.insert_newline();
                                if !modifiers.contains(KeyModifiers::SHIFT) {
                                    app.shift_selection = false;
                                    app.selection_reset();
                                }
                            }
                            KeyCode::Backspace => {
                                app.backspace();
                                if !modifiers.contains(KeyModifiers::SHIFT) {
                                    app.shift_selection = false;
                                    app.selection_reset();
                                }
                            }
                            _ => {}
                        }
                    }
                    Mode::FileTree => {
                        // 数字キーで行番号選択
                        if let KeyCode::Char(ch) = code {
                            if let Some(digit) = ch.to_digit(10) {
                                // digit 1 corresponds to first visible entry (excluding header)
                                let index = digit as usize;
                                let visible = (terminal.size().unwrap().height as usize).saturating_sub(1);
                                let target = app.file_tree.scroll_offset + index;
                                if target < app.file_tree.entries.len() {
                                    app.file_tree.selected = target;
                                    app.file_tree_enter();
                                }
                                continue;
                            }
                        }
                        match code {
                            KeyCode::Up => { app.file_tree_move_up(); }
                            KeyCode::Down => { app.file_tree_move_down(); }
                            KeyCode::Right => { app.file_tree_enter(); }
                            KeyCode::Left => { app.file_tree_go_up(); }
                            KeyCode::Enter => { app.file_tree_enter(); }
                            KeyCode::Char('n') => { app.new_file_popup(); }
                            KeyCode::Char('m') => { app.rename_file_popup(); }
                            KeyCode::Delete => { app.delete_file_popup(); }
                            KeyCode::Char('s') if modifiers == KeyModifiers::CONTROL => { app.save_file(); }
                            _ => {}
                        }
                    }
                }
            }
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    Ok(())
}
