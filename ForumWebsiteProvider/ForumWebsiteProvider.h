#ifndef FORUMWEBSITEPROVIDER_H__
#define FORUMWEBSITEPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ForumWebsiteProvider.h"

// Contract_ForumWebsiteProvider.h pulls in ForumWebsiteProvider/ForumNode.h,
// which pulls in ForumSelf.h and ForumThread.h in turn. Three types, three
// roles, the same three ChessProvider has:
//
//   ForumThread -- a conversation. Knows verbs, not URLs.
//   ForumSelf   -- a SELF. My edges, my record. Reached through my node.
//   ForumNode   -- the host. Owns selves and threads, parses paths, routes,
//                  and holds the one edge that leaves this module.
//
// The node is the centralization artifact: many selves makes it a server, one
// self makes it a peer, and nothing else about the arrangement differs.
//
// ── Two things a reader of ChessProvider should know before reading this ─────
//
// 1. EVERY READ IS A WINDOW. A route handler's reply is one ETCS::Buffer, so no
//    request can return more than bufsize bytes. Chess never noticed. Here,
//    every read verb takes an optional cursor and returns a bounded byte window
//    over a deterministic rendering, ending in "+N" (resume at a line boundary)
//    or "~N" (resumed mid-line, concatenate). One mechanism for thread lists,
//    thread bodies and post text alike.
//
// 2. THE DATABASE EDGE IS A RID, NOT A TYPE. Nothing here includes, links
//    against, or names a DatabaseProvider type -- the connection is a bare RID
//    handed in by the script and invoked by qualified action name through the
//    loader's ridMap, which is the same mechanism HttpServer uses to reach a
//    route target in another module. This module loads fine with no
//    DatabaseProvider present; it just persists nothing.
//
// Buffer API note: ETCS::Buffer reads with restAsString() and writes with
// writeString(const char*), so every write below goes through .c_str().

// ── ForumThread ───────────────────────────────────────────────────────────────
// The scripted/REPL surface. Every one of these serializes on the forum
// ordering stream, so they are safe to call from a pool thread and cannot
// interleave with an HTTP-driven post.

// No token, so this is the TRUSTED caller (script, REPL). Unlike chess, where
// an empty token bypasses seat ownership by design, a post with no identity is
// REFUSED here: a chess move is attributable to the position it produces, while
// a post is attributable only to whoever signed it, and an unsigned row in a
// durable hash chain is a permanent hole nobody can account for.
//
// A script that wants to post therefore has to say as whom. That is a real
// difference from ChessGame::Move and it is the correct one.
DEFINE_WORK_FUNC(ForumThread, Post)
{
    (void)ctx;
    std::string tok, body;
    data >> tok;
    body = data.restAsString();
    data.writeString(self.PostBody(body, tok).c_str());
}

// Read <cursor> -- omit the cursor for the top of the thread.
DEFINE_WORK_FUNC(ForumThread, Read)
{
    (void)ctx;
    data.writeString(self.Read(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ForumThread, Title)
{
    (void)ctx;
    std::string tok;
    data >> tok;
    data.writeString(self.SetTitle(data.restAsString(), tok).c_str());
}

DEFINE_WORK_FUNC(ForumThread, Lock)
{
    (void)ctx;
    data.writeString(self.LockThread(data.restAsString()).c_str());
}

// Tomb <token> <seq>. A removal, recorded rather than hidden: the sequence is
// not renumbered and the chain is not recomputed, so the head still verifies
// and a reader can still see that content of a known hash was here.
DEFINE_WORK_FUNC(ForumThread, Tomb)
{
    (void)ctx;
    std::string tok, seq;
    data >> tok;
    data >> seq;
    data.writeString(self.Tombstone(seq, tok).c_str());
}

DEFINE_WORK_FUNC(ForumThread, Status)
{
    (void)ctx;
    data.writeString(self.StatusLine().c_str());
}

// The whole visible history in sixteen hex digits. THE verb of this module: a
// client that saw head H, and later sees a head that is not H with no new posts
// to explain it, has caught a rewrite -- without holding the thread, without a
// signature, and without trusting anyone to report it.
DEFINE_WORK_FUNC(ForumThread, Head)
{
    (void)ctx;
    data.writeString(self.HeadHash().c_str());
}

// Setup only, and unserialized for the same reason ChessGame::SetMatchKey is:
// AcceptsConcrete reads this field on a pool thread for every request, so it
// must be treated as set-once. If it ever needs to change at runtime, this
// becomes an event.
DEFINE_WORK_FUNC(ForumThread, Key)
{
    (void)ctx;
    std::string k = data.restAsString();
    if (!k.empty()) self.SetThreadKey(k);
    data.writeString(self.ThreadKey().c_str());
}

// Trait-provided verbs, exported. Inheriting a trait supplies the C++ method;
// the dispatch surface is built from work functions, so each still needs a thin
// forwarder. These call the base's PROVIDED method, never the *Concrete impl.
DEFINE_WORK_FUNC(ForumThread, Reset)
{
    (void)ctx;
    data.writeString(self.Reset() ? "reset" : "FAILED");
}

DEFINE_WORK_FUNC(ForumThread, IsActive)
{
    (void)ctx;
    data.writeString(self.IsActive() ? "open" : "locked");
}

DEFINE_WORK_FUNC(ForumThread, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

DEFINE_WORK_FUNC(ForumThread, Filter)
{
    (void)ctx;
    self.Accepts(data);   // empty result == declined, by Filter_'s own contract
}

// ── ForumSelf ─────────────────────────────────────────────────────────────────
// A SELF exports no verbs of its own. It is reached through its node, which is
// also the P2P shape: your self is the one your own node hosts, and there is
// nobody else to address it from. Adding Request/List here would make a self
// addressable two ways, parsing two different URL shapes for the same thing.
DEFINE_WORK_FUNC(ForumSelf, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

DEFINE_WORK_FUNC(ForumSelf, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

// ── ForumNode ─────────────────────────────────────────────────────────────────
// THE http entry point. One route registers against this pair (Request +
// Filter) and everything else is resolved behind it by path.
DEFINE_WORK_FUNC(ForumNode, Request)
{
    // Captured here rather than at construction, and on every request rather
    // than once: a persisted write triggered later by reaping has no request of
    // its own, so it runs under the last authority that actually asked for
    // something. Same reasoning as HttpServer::Start capturing at Start.
    self.SetRunContext(ctx);
    data.writeString(self.Request(data.restAsString()).c_str());
}

// Every thread on this node, windowed. The operator view and the front page,
// same call.
DEFINE_WORK_FUNC(ForumNode, Index)
{
    self.SetRunContext(ctx);
    data.writeString(self.Index(data.restAsString()).c_str());
}

// Every self, windowed. A self exists only once somebody has been here, so this
// is the only enumeration of participants that exists.
DEFINE_WORK_FUNC(ForumNode, Selves)
{
    self.SetRunContext(ctx);
    data.writeString(self.Selves(data.restAsString()).c_str());
}

// Recompute every content hash and every chain link from scratch and report
// disagreements. Cheap enough to run from the shell, and the check that makes
// the load path trustworthy: a row that came back off disk wrong, or a body
// whose chunks reassembled short, shows up here rather than silently becoming
// the new truth.
DEFINE_WORK_FUNC(ForumNode, Verify)
{
    self.SetRunContext(ctx);
    data.writeString(self.Verify(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ForumNode, Mount)
{
    (void)ctx;
    std::string m = data.restAsString();
    if (!m.empty()) self.SetMount(m);
    data.writeString(self.MountPath().c_str());
}

// Db <rid> [<Action>] -- the ONE place a RID crosses into this module from
// outside, explicit for exactly that reason. Everything else here is
// parent/child.
//
// The action defaults to ExecuteRaw, which is LocalDatabase's own. Overridable
// because the target only has to accept "a SQL statement in a Buffer" -- any
// type offering that shape works, and this module deliberately does not know
// which one it got.
DEFINE_WORK_FUNC(ForumNode, Db)
{
    self.SetRunContext(ctx);
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;
    if (rid == 0)
    {
        ETCS_LOG("ForumNode::Db", "expected '<rid> [<Action>]' -- got: " << data.buf);
        return;
    }
    self.SetDb(rid, action);
    data.reset();
    data << rid;
}

// Issue this module's schema over that edge. Separate statements, not one blob
// handed to InitializeSchema: the blob would not fit a single Buffer, so the
// same wire budget that shapes the read path shapes the DDL.
DEFINE_WORK_FUNC(ForumNode, InitDb)
{
    self.SetRunContext(ctx);
    data.writeString(self.InitDb().c_str());
}

// The subscriber ConnectionManager calls per accepted connection. Takes only
// Buffer data, never a NetworkProvider type: this module does not link against
// another module's types, which is what keeps ForumWebsiteProvider loadable
// with no NetworkProvider present at all.
//
// Nothing durable happens here. A connection is per-request under
// Connection: close, so arriving means nothing -- a self is created on the
// first request that names one, and locked on the first post.
DEFINE_WORK_FUNC(ForumNode, Accept)
{
    self.SetRunContext(ctx);
    ETCS_LOG("ForumNode", "Accept: connection joined, mount '" << self.MountPath() << "'");
    data.writeString(self.Index("").c_str());
}

DEFINE_WORK_FUNC(ForumNode, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

DEFINE_WORK_FUNC(ForumNode, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

// LoadRows -- the consuming half of a pair with LocalDatabase's RowProduce.
// The script wires them as a causal DAG:
//
//   db.RowProduce "SELECT * FROM forum_thread" -> forum.LoadRows
//   db.RowProduce "SELECT * FROM forum_post"   -> forum.LoadRows
//   db.RowProduce "SELECT * FROM forum_body"   -> forum.LoadRows
//
// That ORDER is a precondition: a body chunk whose post has not arrived is
// dropped with a log rather than buffered, because buffering it would mean
// holding unattributed content of unknown provenance in memory.
//
// RowProduce, not QueryProduce. QueryProduce emits SQL statements -- right for
// database-to-database mirroring, where the consumer is another sqlite handle
// that can execute them, and wrong for anything else, which then has to parse
// SQL back into fields. RowProduce sends a column frame and then records, so
// the mapping is by name and this module never sees a statement.
//
// Run Verify afterwards. That is what makes this path trustworthy rather than
// merely convenient.
DEFINE_STREAM_FUNC_CONSUME(ForumNode, LoadRows)
{
    (void)data;
    self.SetRunContext(ctx);

    int applied = 0, skipped = 0;
    ETCS::Buffer frame;
    while (stream.readRaw(frame))
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        const std::string stmt = frame.toString();
        if (stmt.empty()) continue;

        const std::string r = self.LoadRow(stmt);
        if (r == "THREAD" || r == "POST" || r == "BODY") ++applied;
        else
        {
            ++skipped;
            if (r != "SKIP")
                ETCS_LOG("ForumNode::LoadRows", r << " on: " << stmt.substr(0, 96));
        }
    }

    ETCS_LOG("ForumNode::LoadRows", "load complete: " << applied
             << " rows applied, " << skipped << " skipped. Run Verify.");
}

#endif // FORUMWEBSITEPROVIDER_H__