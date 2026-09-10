(* S426 probe: simplif.ml's `enabled` (simplif.ml:838) -- two root or-rows over
   a record, whose column-level do_split moves the trailing `{local =
   Never_local; _}` row BEFORE the or-rows (safe_before) and compiles the
   or-handlers `_ Default_local -> exit` under a ctx that has already proven
   `local`.  The whole-match licence (a proven-Total match has no bottom
   default entry, so a miss with no compatible entry gets no clause) drops the
   fake-deid holes those handlers would otherwise carry; the attempt
   acceptance tests must still see them as fails, or the RELAXED or-reading
   (hoist past the or-row, accepted when it invents no fail) wins and re-tests
   `local` in a handler.  NOTPDROPS=1 reverts the accounting alone (18/30);
   NOTPALL=1 reverts the licence (0/0, the S425 reading). *)
type inline_attribute =
  | Always_inline | Never_inline | Hint_inline | Unroll of int | Default_inline
type local_attribute = Always_local | Never_local | Default_local
type attr = { inline : inline_attribute; stub : bool; local : local_attribute }

let enabled = function
  | {local = Always_local; _}
  | {local = Default_local; inline = (Never_inline | Default_inline); _}
    -> true
  | {local = Default_local;
     inline = (Always_inline | Unroll _ | Hint_inline); _}
  | {local = Never_local; _}
    -> false

let () =
  List.iter (fun local ->
      List.iter (fun inline ->
          print_string
            (if enabled {inline; stub = false; local} then "T" else "F"))
        [Always_inline; Never_inline; Hint_inline; Unroll 3; Default_inline];
      print_newline ())
    [Always_local; Never_local; Default_local]
