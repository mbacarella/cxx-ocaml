// Port of bytecomp/printinstr.ml (TYPECHECKER.md stage 10): pretty-print
// lists of instructions (-dinstr).
#include "cppcaml/typing/printinstr.hpp"

#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing::printinstr {

using namespace instruct;
using format::Formatter;
using format::fprintf;
using format::pr;

void instruction(Formatter& ppf, const Instruction& i) {
  switch (i.k) {
    case IK::Klabel: fprintf(ppf, "L%i:", i.n); return;
    case IK::Kacc: fprintf(ppf, "\tacc %i", i.n); return;
    case IK::Kenvacc: fprintf(ppf, "\tenvacc %i", i.n); return;
    case IK::Kpush: fprintf(ppf, "\tpush"); return;
    case IK::Kpop: fprintf(ppf, "\tpop %i", i.n); return;
    case IK::Kassign: fprintf(ppf, "\tassign %i", i.n); return;
    case IK::Kpush_retaddr: fprintf(ppf, "\tpush_retaddr L%i", i.n); return;
    case IK::Kapply: fprintf(ppf, "\tapply %i", i.n); return;
    case IK::Kappterm: fprintf(ppf, "\tappterm %i, %i", i.n, i.m); return;
    case IK::Kreturn: fprintf(ppf, "\treturn %i", i.n); return;
    case IK::Krestart: fprintf(ppf, "\trestart"); return;
    case IK::Kgrab: fprintf(ppf, "\tgrab %i", i.n); return;
    case IK::Kclosure: fprintf(ppf, "\tclosure L%i, %i", i.n, i.m); return;
    case IK::Kclosurerec:
      fprintf(ppf, "\tclosurerec");
      for (const ClosureLabel& c : i.closures) fprintf(ppf, " %i", c.lbl);
      fprintf(ppf, ", %i", i.n);
      return;
    case IK::Koffsetclosure: fprintf(ppf, "\toffsetclosure %i", i.n); return;
    case IK::Kgetglobal: fprintf(ppf, "\tgetglobal %a", pr(printlambda::ident, i.id)); return;
    case IK::Ksetglobal: fprintf(ppf, "\tsetglobal %a", pr(printlambda::ident, i.id)); return;
    case IK::Kconst:
      fprintf(ppf, "@[<10>\tconst@ %a@]", pr(printlambda::structured_constant, i.cst));
      return;
    case IK::Kmakeblock: fprintf(ppf, "\tmakeblock %i, %i", i.n, i.m); return;
    case IK::Kmakefloatblock: fprintf(ppf, "\tmakefloatblock %i", i.n); return;
    case IK::Kgetfield: fprintf(ppf, "\tgetfield %i", i.n); return;
    case IK::Ksetfield: fprintf(ppf, "\tsetfield %i", i.n); return;
    case IK::Kgetfloatfield: fprintf(ppf, "\tgetfloatfield %i", i.n); return;
    case IK::Ksetfloatfield: fprintf(ppf, "\tsetfloatfield %i", i.n); return;
    case IK::Kvectlength: fprintf(ppf, "\tvectlength"); return;
    case IK::Kgetvectitem: fprintf(ppf, "\tgetvectitem"); return;
    case IK::Ksetvectitem: fprintf(ppf, "\tsetvectitem"); return;
    case IK::Kgetstringchar: fprintf(ppf, "\tgetstringchar"); return;
    case IK::Kgetbyteschar: fprintf(ppf, "\tgetbyteschar"); return;
    case IK::Ksetbyteschar: fprintf(ppf, "\tsetbyteschar"); return;
    case IK::Kbranch: fprintf(ppf, "\tbranch L%i", i.n); return;
    case IK::Kbranchif: fprintf(ppf, "\tbranchif L%i", i.n); return;
    case IK::Kbranchifnot: fprintf(ppf, "\tbranchifnot L%i", i.n); return;
    case IK::Kstrictbranchif: fprintf(ppf, "\tstrictbranchif L%i", i.n); return;
    case IK::Kstrictbranchifnot: fprintf(ppf, "\tstrictbranchifnot L%i", i.n); return;
    case IK::Kswitch: {
      auto labels = [](Formatter& f, Slice<label> labs) {
        for (label l : labs) fprintf(f, "@ %i", l);
      };
      fprintf(ppf, "@[<10>\tswitch%a/%a@]", pr(labels, i.sw_consts), pr(labels, i.sw_blocks));
      return;
    }
    case IK::Kboolnot: fprintf(ppf, "\tboolnot"); return;
    case IK::Kpushtrap: fprintf(ppf, "\tpushtrap L%i", i.n); return;
    case IK::Kpoptrap: fprintf(ppf, "\tpoptrap"); return;
    case IK::Kraise: fprintf(ppf, "\t%s", lambda::raise_kind(i.raise)); return;
    case IK::Kcheck_signals: fprintf(ppf, "\tcheck_signals"); return;
    case IK::Kccall: fprintf(ppf, "\tccall %s, %i", i.prim, i.n); return;
    case IK::Knegint: fprintf(ppf, "\tnegint"); return;
    case IK::Kaddint: fprintf(ppf, "\taddint"); return;
    case IK::Ksubint: fprintf(ppf, "\tsubint"); return;
    case IK::Kmulint: fprintf(ppf, "\tmulint"); return;
    case IK::Kdivint: fprintf(ppf, "\tdivint"); return;
    case IK::Kmodint: fprintf(ppf, "\tmodint"); return;
    case IK::Kandint: fprintf(ppf, "\tandint"); return;
    case IK::Korint: fprintf(ppf, "\torint"); return;
    case IK::Kxorint: fprintf(ppf, "\txorint"); return;
    case IK::Klslint: fprintf(ppf, "\tlslint"); return;
    case IK::Klsrint: fprintf(ppf, "\tlsrint"); return;
    case IK::Kasrint: fprintf(ppf, "\tasrint"); return;
    case IK::Kintcomp:
      switch (i.icmp) {
        case lambda::IntegerComparison::Ceq: fprintf(ppf, "\teqint"); return;
        case lambda::IntegerComparison::Cne: fprintf(ppf, "\tneqint"); return;
        case lambda::IntegerComparison::Clt: fprintf(ppf, "\tltint"); return;
        case lambda::IntegerComparison::Cgt: fprintf(ppf, "\tgtint"); return;
        case lambda::IntegerComparison::Cle: fprintf(ppf, "\tleint"); return;
        case lambda::IntegerComparison::Cge: fprintf(ppf, "\tgeint"); return;
      }
      return;
    case IK::Kphyscomp:
      fprintf(ppf, i.pcmp == lambda::PhysicalComparison::CPeq ? "\tphyseq" : "\tphysneq");
      return;
    case IK::Koffsetint: fprintf(ppf, "\toffsetint %i", i.n); return;
    case IK::Koffsetref: fprintf(ppf, "\toffsetref %i", i.n); return;
    case IK::Kisint: fprintf(ppf, "\tisint"); return;
    case IK::Kisout: fprintf(ppf, "\tisout"); return;
    case IK::Kgetmethod: fprintf(ppf, "\tgetmethod"); return;
    case IK::Kgetpubmet: fprintf(ppf, "\tgetpubmet %i", i.n); return;
    case IK::Kgetdynmet: fprintf(ppf, "\tgetdynmet"); return;
    case IK::Kperform: fprintf(ppf, "\tperform"); return;
    case IK::Kresume: fprintf(ppf, "\tresume"); return;
    case IK::Kresumeterm: fprintf(ppf, "\tresumeterm %i", i.n); return;
    case IK::Kreperformterm: fprintf(ppf, "\treperformterm %i", i.n); return;
    case IK::Kstop: fprintf(ppf, "\tstop"); return;
    case IK::Kevent: {
      const Location& l = i.event->ev_loc;
      fprintf(ppf, "\tevent \"%s\" %i-%i", l.loc_start.pos_fname, l.loc_start.pos_cnum, l.loc_end.pos_cnum);
      return;
    }
  }
}

// instruction_list: `Klabel lbl :: il -> "L%i:%a"`, `instr :: il -> "%a@ %a"`
// -- the recursion unrolled (the lists are long), printing the same items in
// the same order.
static void instruction_list(Formatter& ppf, code c) {
  for (; c; c = c->tl) {
    if (c->hd.k == IK::Klabel)
      fprintf(ppf, "L%i:", c->hd.n);
    else
      fprintf(ppf, "%a@ ", pr(instruction, c->hd));
  }
}

void instrlist(Formatter& ppf, code c) { fprintf(ppf, "@[<v 0>%a@]", pr(instruction_list, c)); }

std::string dump(code c) {
  Formatter ppf;
  fprintf(ppf, "%a@.", pr(instrlist, c));
  return ppf.take();
}

}  // namespace cppcaml::typing::printinstr
