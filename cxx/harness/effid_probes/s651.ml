(* S429 probe: mk_failaction_pos reads the jump context only while the number
   of MISSING constructors is BELOW `Clflags.match_context_rows` (32); past
   that it drops to mk_failaction_neg, which ignores the context outright and
   sends every gap to the first default entry (matching.ml:3001).  pprintast's
   `expression` (pprintast.ml:832) is exactly there -- 34 Pexp_ constructors,
   and the division reached from the `Pexp_ifthenelse _ | Pexp_sequence _`
   or-handler's failed guard names two of them, so its 32 fail patterns keep
   the `default` clause however narrow the context is; without the cap the
   S429 context made that switch complete and dropped it.
     RESIDUAL 6/98, unchanged by S429 (identical under the revert hooks): the
   OTHER half of the same rule is still open -- under the cap the gaps must go
   to the FIRST default entry whatever its matrix says, where we still pick
   per-gap the first entry the arrival is compatible with, so our first switch
   sends them straight to the catch-all instead of through this division. *)
type t =
    C0 of int
  | C1 of int
  | C2 of int
  | C3 of int
  | C4 of int
  | C5 of int
  | C6 of int
  | C7 of int
  | C8 of int
  | C9 of int
  | C10 of int
  | C11 of int
  | C12 of int
  | C13 of int
  | C14 of int
  | C15 of int
  | C16 of int
  | C17 of int
  | C18 of int
  | C19 of int
  | C20 of int
  | C21 of int
  | C22 of int
  | C23 of int
  | C24 of int
  | C25 of int
  | C26 of int
  | C27 of int
  | C28 of int
  | C29 of int
  | C30 of int
  | C31 of int
  | C32 of int
  | C33 of int

let other x = match x with C0 n -> n | _ -> 7

let rec pr g x =
  match x with
  | (C14 _ | C15 _) when g -> 100 + pr false x
  | C14 n -> n + 1
  | C15 n -> n + 2
  | _ -> other x

let all =
  [ C0 0;
    C1 1;
    C2 2;
    C3 3;
    C4 4;
    C5 5;
    C6 6;
    C7 7;
    C8 8;
    C9 9;
    C10 10;
    C11 11;
    C12 12;
    C13 13;
    C14 14;
    C15 15;
    C16 16;
    C17 17;
    C18 18;
    C19 19;
    C20 20;
    C21 21;
    C22 22;
    C23 23;
    C24 24;
    C25 25;
    C26 26;
    C27 27;
    C28 28;
    C29 29;
    C30 30;
    C31 31;
    C32 32;
    C33 33 ]

let () =
  List.iter
    (fun g ->
      List.iter (fun x -> print_string (string_of_int (pr g x))) all;
      print_newline ())
    [ false; true ]
