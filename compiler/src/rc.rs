// Inserts reference counting into MIR from liveness.
//
// Rule: a local owns its value from its definition to its last use.
// - A consuming use that isn't the last use gets a `Dup` before it.
// - A borrowing last use gets a `Drop` after it.
// - A value that is never used is dropped right after its definition.
// - On a control-flow edge, values live at the end of the predecessor but
//   not needed by the successor are dropped on that edge.

use crate::mir::*;
use crate::types::{Program, Ty, TypeKind};
use std::collections::HashSet;

pub fn needs_rc(t: &Ty, p: &Program) -> bool {
    fn go(t: &Ty, p: &Program, seen: &mut HashSet<Ty>) -> bool {
        match t {
            Ty::Int | Ty::Float | Ty::Bool | Ty::Unit | Ty::Never | Ty::Err => false,
            Ty::Text | Ty::Func(_) | Ty::Iface(_) => true,
            Ty::Opt(x) => go(x, p, seen),
            Ty::Param(_) | Ty::Var(_) => true,
            Ty::Adt(d, args) => {
                if !seen.insert(t.clone()) {
                    // recursive types are boxed
                    return true;
                }
                let r = match &p.defs[*d].kind {
                    TypeKind::Builtin => true, // List, Map, Set
                    TypeKind::Record { fields } => fields.iter().any(|f| go(&f.ty.subst(args), p, seen)),
                    TypeKind::Enum { variants } => variants.iter().any(|v| v.fields.iter().any(|f| go(&f.ty.subst(args), p, seen))),
                    TypeKind::Newtype(inner) => go(inner, p, seen),
                    TypeKind::Interface { .. } => true,
                };
                seen.remove(t);
                r
            }
        }
    }
    go(t, p, &mut HashSet::new())
}

type Set = Vec<bool>;

fn union_into(a: &mut Set, b: &Set) -> bool {
    let mut changed = false;
    for i in 0..a.len() {
        if b[i] && !a[i] {
            a[i] = true;
            changed = true;
        }
    }
    changed
}

pub fn insert_rc(f: &mut Func, prog: &Program) {
    let n = f.locals.len();
    let rc: Vec<bool> = f.locals.iter().map(|l| !l.self_ptr && needs_rc(&l.ty, prog)).collect();
    let nb = f.blocks.len();

    // use/def per block (upward-exposed uses)
    let mut gen_ = vec![vec![false; n]; nb];
    let mut kill = vec![vec![false; n]; nb];
    for (bi, b) in f.blocks.iter().enumerate() {
        let (g, k) = (&mut gen_[bi], &mut kill[bi]);
        let mut uses = vec![];
        let mut defs = vec![];
        for s in &b.stmts {
            uses.clear();
            defs.clear();
            s.uses(&mut uses);
            s.defs(&mut defs);
            for (l, _) in &uses {
                if !k[*l] {
                    g[*l] = true;
                }
            }
            for d in &defs {
                k[*d] = true;
            }
        }
        uses.clear();
        b.term.uses(&mut uses);
        for (l, _) in &uses {
            if !k[*l] {
                g[*l] = true;
            }
        }
    }
    let succs: Vec<Vec<B>> = f.blocks.iter().map(|b| b.term.succs()).collect();
    let mut live_in = vec![vec![false; n]; nb];
    let mut live_out = vec![vec![false; n]; nb];
    let mut changed = true;
    while changed {
        changed = false;
        for bi in (0..nb).rev() {
            let mut out = vec![false; n];
            for s in &succs[bi] {
                union_into(&mut out, &live_in[*s]);
            }
            let mut inn = gen_[bi].clone();
            for i in 0..n {
                if out[i] && !kill[bi][i] {
                    inn[i] = true;
                }
            }
            if inn != live_in[bi] {
                live_in[bi] = inn;
                changed = true;
            }
            live_out[bi] = out;
        }
    }

    // rewrite each block
    let mut term_live: Vec<Set> = vec![];
    for bi in 0..nb {
        let mut live = live_out[bi].clone();
        let blk = &f.blocks[bi];
        // terminator
        let mut tuses = vec![];
        blk.term.uses(&mut tuses);
        let mut tl = live.clone();
        let mut term_pre = vec![];
        for (l, consume) in &tuses {
            if !rc[*l] {
                continue;
            }
            if *consume {
                // Return/Throw: nothing is live after them
                if live[*l] {
                    term_pre.push(Stmt::Dup(*l));
                }
            } else {
                tl[*l] = true;
            }
        }
        term_live.push(tl);
        for (l, _) in &tuses {
            live[*l] = true;
        }
        let mut out_stmts: Vec<Vec<Stmt>> = vec![];
        let stmts = blk.stmts.clone();
        for s in stmts.iter().rev() {
            let mut uses = vec![];
            let mut defs = vec![];
            s.uses(&mut uses);
            s.defs(&mut defs);
            let mut after_uses = live.clone();
            for d in &defs {
                after_uses[*d] = false;
            }
            let mut pre = vec![];
            let mut post = vec![];
            let mut seen: Vec<usize> = vec![];
            for (l, _) in &uses {
                if !rc[*l] || seen.contains(l) {
                    continue;
                }
                seen.push(*l);
                let c = uses.iter().filter(|(x, cons)| x == l && *cons).count();
                let b = uses.iter().filter(|(x, cons)| x == l && !*cons).count();
                if after_uses[*l] {
                    for _ in 0..c {
                        pre.push(Stmt::Dup(*l));
                    }
                } else if c >= 1 {
                    let dups = if b >= 1 { c } else { c - 1 };
                    for _ in 0..dups {
                        pre.push(Stmt::Dup(*l));
                    }
                    if b >= 1 {
                        debug_assert!(!defs.contains(l), "borrowed and redefined in one statement");
                        post.push(Stmt::Drop(*l));
                    }
                } else {
                    debug_assert!(!defs.contains(l), "borrowed and redefined in one statement");
                    post.push(Stmt::Drop(*l));
                }
            }
            for d in &defs {
                if rc[*d] && !live[*d] {
                    post.push(Stmt::Drop(*d));
                }
            }
            for d in &defs {
                live[*d] = false;
            }
            for (l, _) in &uses {
                live[*l] = true;
            }
            let mut group = pre;
            group.push(s.clone());
            group.extend(post);
            out_stmts.push(group);
        }
        out_stmts.reverse();
        let mut new_stmts: Vec<Stmt> = out_stmts.into_iter().flatten().collect();
        new_stmts.extend(term_pre);
        f.blocks[bi].stmts = new_stmts;
    }

    // parameters that are never used
    let mut entry_drops = vec![];
    for p in &f.params {
        if rc[*p] && !live_in[0][*p] {
            entry_drops.push(Stmt::Drop(*p));
        }
    }
    // closure environment loads count as definitions, handled above

    // edge drops
    let mut preds = vec![0usize; nb];
    for s in &succs {
        for t in s {
            preds[*t] += 1;
        }
    }
    let mut new_blocks: Vec<Block> = vec![];
    for bi in 0..nb {
        let ss = succs[bi].clone();
        for (k, s) in ss.iter().enumerate() {
            let drops: Vec<Stmt> = (0..n).filter(|l| rc[*l] && term_live[bi][*l] && !live_in[*s][*l]).map(Stmt::Drop).collect();
            if drops.is_empty() {
                continue;
            }
            if ss.len() == 1 {
                f.blocks[bi].stmts.extend(drops);
            } else if preds[*s] == 1 {
                let mut st = drops;
                st.extend(f.blocks[*s].stmts.drain(..));
                f.blocks[*s].stmts = st;
            } else {
                let nbi = nb + new_blocks.len();
                let line = f.blocks[bi].line;
                new_blocks.push(Block { stmts: drops, term: Term::Goto(*s), line });
                let mut targets = f.blocks[bi].term.succs_mut();
                *targets[k] = nbi;
            }
        }
    }
    f.blocks.extend(new_blocks);
    if !entry_drops.is_empty() {
        let mut st = entry_drops;
        st.extend(f.blocks[0].stmts.drain(..));
        f.blocks[0].stmts = st;
    }
}
