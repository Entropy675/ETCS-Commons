#ifndef CHESSPROVIDER_H__
#define CHESSPROVIDER_H__


#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ChessProvider.h"

// Four types:
//
//   ChessGame  -- a board. The record's judge on the host, a follower of the
//                 record everywhere else, its own judge alone (its header).
//   ChessShare -- this runtime's part in a game between runtimes: who is
//                 here, whose board is whose, the word to the page.
//   ChessBoard -- the game as pixels, in the drawable tree of a window or a
//                 canvas; the one picture of it on either substrate.
//   ChessTable -- the seat: clicks and keys in, Act out; the OS bars.
//
// Buffer API note: ETCS::Buffer reads with restAsString() and writes with
// writeString(const char*), so every write below goes through .c_str().

// ── ChessGame ─────────────────────────────────────────────────────────────────
// Act <as> <verb> [<arg>] -- move, sit, say, resign, draw, decline, leave,
// reset. `as` is the hotseat's colour ("white"/"black"); in a session this
// board's own name, whatever is written. Answers as the board does, or SENT
// from a follower (the record answers, through Absorb).
DEFINE_WORK_FUNC(ChessGame, Act)
{
    (void)ctx;
    std::string as, verb;
    data >> as >> verb;
    std::string arg = data.restAsString();
    while (!arg.empty() && (arg.front() == ' ' || arg.front() == ',')) arg.erase(0, 1);
    data.writeString(self.Act(as, verb, arg).c_str());
}
// Move <as> <uci> -- Act's most used form.
DEFINE_WORK_FUNC(ChessGame, Move)
{
    (void)ctx;
    std::string as, uci;
    data >> as >> uci;
    data.writeString(self.Act(as, "move", uci).c_str());
}
DEFINE_WORK_FUNC(ChessGame, Fen)     { (void)ctx; (void)data; data.writeString(self.Fen().c_str()); }
DEFINE_WORK_FUNC(ChessGame, LoadFen) { (void)ctx; data.writeString(self.LoadFenStr(data.restAsString()).c_str()); }
// Status [<as>] -- "side state role wSeat bSeat offer whiteName blackName".
DEFINE_WORK_FUNC(ChessGame, Status)  { (void)ctx; std::string as; data >> as; data.writeString(self.StatusLine(as).c_str()); }
// Chat [<from>] -- a page of the log ("<base> <next>" then lines); the tail with no cursor.
DEFINE_WORK_FUNC(ChessGame, Chat)    { (void)ctx; std::string from; data >> from; data.writeString(self.ChatPage(from).c_str()); }
// Tail [<n>] -- the last n lines of the conversation (12 by default).
DEFINE_WORK_FUNC(ChessGame, Tail)    { (void)ctx; uint32_t n = 0; data >> n; data.writeString(self.ChatTail(n ? n : 12).c_str()); }
// History [<from>] -- a page of "<uci> <fen>" plies.
DEFINE_WORK_FUNC(ChessGame, History) { (void)ctx; std::string from; data >> from; data.writeString(self.HistoryPage(from).c_str()); }
DEFINE_WORK_FUNC(ChessGame, Seats)   { (void)ctx; (void)data; data.writeString(self.Seats().c_str()); }
// Hash -- "<hash> <seq>": the board, and the record seq it is the board of.
DEFINE_WORK_FUNC(ChessGame, Hash)    { (void)ctx; (void)data; data.writeString(self.Hash().c_str()); }
DEFINE_WORK_FUNC(ChessGame, Reset)   { (void)ctx; (void)data; data.writeString(self.Reset() ? self.Fen().c_str() : "FAILED"); }
DEFINE_WORK_FUNC(ChessGame, IsActive){ (void)ctx; (void)data; data.writeString(self.IsActive() ? "active" : "finished"); }
DEFINE_WORK_FUNC(ChessGame, Delete)  { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// game.Emit() -> proposals.Take() -- a follower's proposals, one line each,
// for as long as the session lasts. Ends when the reader goes.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(ChessGame, Emit)
{
    (void)data;
    std::string line;
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        if (!self.nextEmit(line)) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        if (!stream.writeMessage(line)) break;
    }
}
// record.Follow(0) -> game.Absorb() -- the record, applied in its order.
DEFINE_STREAM_FUNC_CONSUME(ChessGame, Absorb)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.Absorb(m);
    ETCS_LOG("ChessGame", "Absorb: the record ended.");
}
// proposals.Follow(0) -> game.Judge() -- the host's board judging every line
// proposed to it, into the record.
DEFINE_STREAM_FUNC_CONSUME(ChessGame, Judge)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.JudgeLine(m);
}

// ── ChessShare ────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC_TYPED(ChessShare, Attach, (ETCS::RID, game)) { (void)ctx; self.Attach(game); }
// Host <record> <proposals> <presence> <name> [<hall> <kind> <id>] -- the
// hall (a Lobby) the game is advertised in, as <kind> under <id>.
DEFINE_WORK_FUNC_TYPED(ChessShare, Host, (ETCS::RID, record), (ETCS::RID, proposals), (ETCS::RID, presence),
                       (std::string, name), (ETCS::RID, hall), (std::string, kind), (std::string, id))
{
    (void)ctx;
    self.Host(record, proposals, presence, name, hall, kind, id);
}
// Serve <record> <proposals> <presence> <room> <links> <hall> -- a table this
// runtime hosts and judges, named here (ChessShare::Serve).
DEFINE_WORK_FUNC_TYPED(ChessShare, Serve, (ETCS::RID, record), (ETCS::RID, proposals), (ETCS::RID, presence),
                       (ETCS::RID, room), (ETCS::RID, links), (ETCS::RID, hall))
{
    (void)ctx;
    self.Serve(record, proposals, presence, room, links, hall);
}
// Join <presence> <name> <owner>
DEFINE_WORK_FUNC_TYPED(ChessShare, Join, (ETCS::RID, presence), (std::string, name), (std::string, owner))
{
    (void)ctx;
    self.Join(presence, name, owner);
}
DEFINE_WORK_FUNC(ChessShare, Tick)   { (void)ctx; (void)data; self.Tick(); }
DEFINE_WORK_FUNC(ChessShare, Leave)  { (void)ctx; (void)data; self.Leave(); }
// Who -- "<name> <seat> [host]" per line.
DEFINE_WORK_FUNC(ChessShare, Who)    { (void)ctx; (void)data; data.writeString(self.Who().c_str()); }
DEFINE_WORK_FUNC(ChessShare, Delete) { (void)ctx; (void)data; self.Delete(); }
// presence.Watch() -> share.Roster() -- the listing, whole, on every change.
DEFINE_STREAM_FUNC_CONSUME(ChessShare, Roster)
{
    (void)data;
    std::string listing;
    while (!ctx.isInterrupted() && stream.readMessage(listing)) self.roster(listing);
}
// proposals.Follow(0) -> share.Judge() -- the host's judge stream, with the
// hall and presence kept current behind it (ChessShare::judge).
DEFINE_STREAM_FUNC_CONSUME(ChessShare, Judge)
{
    (void)data;
    std::string line;
    while (!ctx.isInterrupted() && stream.readMessage(line)) self.judge(line);
}

// ── ChessBoard ────────────────────────────────────────────────────────────────
// Create <size_px> -- the raster, a multiple of eight; 704 by default.
DEFINE_WORK_FUNC_TYPED(ChessBoard, Create, (uint32_t, size_px))
{
    (void)ctx;
    self.Create(size_px);
    ETCS_LOG("ChessBoard::Create", self.Size() << "px, " << self.SquarePx() << " a square, on RID:" << self.getRID());
}
DEFINE_WORK_FUNC_TYPED(ChessBoard, Bind, (ETCS::RID, game))         { (void)ctx; self.Bind(game); }
DEFINE_WORK_FUNC_TYPED(ChessBoard, SetPosition, (int32_t, x), (int32_t, y)) { (void)ctx; self.SetPosition(x, y); }
DEFINE_WORK_FUNC_TYPED(ChessBoard, SetOrder, (int32_t, z))          { (void)ctx; self.SetOrder(z); }
DEFINE_WORK_FUNC_TYPED(ChessBoard, SetHidden, (int32_t, hidden))    { (void)ctx; self.SetHidden(hidden != 0); }
DEFINE_WORK_FUNC_TYPED(ChessBoard, SetFlip, (int32_t, flip))        { (void)ctx; self.SetFlip(flip != 0); }
// Select <square> -- drawn as picked; no square clears it.
DEFINE_WORK_FUNC(ChessBoard, Select)  { (void)ctx; std::string sq; data >> sq; self.Select(sq); }
// SquareAt <x> <y> -- in the raster's own space.
DEFINE_WORK_FUNC(ChessBoard, SquareAt)
{
    (void)ctx;
    int32_t x = 0, y = 0;
    data >> x >> y;
    data.writeString(self.SquareAt(x, y).c_str());
}
DEFINE_WORK_FUNC(ChessBoard, Delete)  { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// ── ChessTable ────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(ChessTable, Create)                                   { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(ChessTable, BindGame,  (ETCS::RID, game))       { (void)ctx; self.BindGame(game); }
DEFINE_WORK_FUNC_TYPED(ChessTable, BindShare, (ETCS::RID, share))      { (void)ctx; self.BindShare(share); }
DEFINE_WORK_FUNC_TYPED(ChessTable, BindBoard, (ETCS::RID, board))      { (void)ctx; self.BindBoard(board); }
// BindStatus <turn> <seats> <note> -- three labels.
DEFINE_WORK_FUNC_TYPED(ChessTable, BindStatus, (ETCS::RID, turn), (ETCS::RID, seats), (ETCS::RID, note))
{
    (void)ctx;
    self.BindStatus(turn, seats, note);
}
// BindBars <chat_pane> <cmd_pane> <shown_x> <hidden_x> -- the two panes
// that slide in from the right (Tab).
DEFINE_WORK_FUNC_TYPED(ChessTable, BindBars, (ETCS::RID, chat), (ETCS::RID, cmd), (int32_t, shown_x), (int32_t, hidden_x))
{
    (void)ctx;
    self.BindBars(chat, cmd, shown_x, hidden_x);
}
DEFINE_WORK_FUNC_TYPED(ChessTable, BindLine, (ETCS::RID, label))       { (void)ctx; self.BindLine(label); }
DEFINE_WORK_FUNC_TYPED(ChessTable, ChatRow,  (ETCS::RID, label))       { (void)ctx; self.ChatRow(label); }
// Click <x> <y> -- a press at a window point, for a test or a page.
DEFINE_WORK_FUNC_TYPED(ChessTable, Click, (int32_t, x), (int32_t, y))  { (void)ctx; self.Click(x, y); }
// Key <code> -- a GLFW key, pressed then released.
DEFINE_WORK_FUNC_TYPED(ChessTable, Key, (uint32_t, code))
{
    (void)ctx;
    self.Key(static_cast<uint16_t>(code), true);
    self.Key(static_cast<uint16_t>(code), false);
}
DEFINE_WORK_FUNC(ChessTable, Type)   { (void)ctx; self.Type(data.restAsString()); }
DEFINE_WORK_FUNC(ChessTable, Submit) { (void)ctx; (void)data; self.Submit(); }
// Do <line> -- a whole line typed on the page, sent as if Enter followed.
DEFINE_WORK_FUNC(ChessTable, Do)     { (void)ctx; self.Do(data.restAsString()); }
DEFINE_WORK_FUNC(ChessTable, Toggle) { (void)ctx; (void)data; self.Toggle(); }
DEFINE_WORK_FUNC(ChessTable, Step)   { (void)ctx; (void)data; self.Step(); }
DEFINE_WORK_FUNC(ChessTable, Report) { (void)ctx; (void)data; data.writeString(self.Report().c_str()); }
DEFINE_WORK_FUNC(ChessTable, Delete) { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// main.ProducePointer() -> table.ConsumePointer() -- a left press is a click.
DEFINE_STREAM_FUNC_CONSUME(ChessTable, ConsumePointer)
{
    (void)data;
    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;
        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_BUTTON_DOWN && ev.key == 0) self.Click(ev.x, ev.y);
    }
}
// main.ProduceEvents() -> table.ConsumeKeys() -- the keyboard, for the bars.
DEFINE_STREAM_FUNC_CONSUME(ChessTable, ConsumeKeys)
{
    (void)data;
    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;
        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_DOWN)    self.Key(ev.key, true);
        else if (ev.action == INPUT_UP) self.Key(ev.key, false);
    }
}

#endif // CHESSPROVIDER_H__
