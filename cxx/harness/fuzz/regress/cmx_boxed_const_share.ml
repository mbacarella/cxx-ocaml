(* FUZZ: levels=nat
   Found by fuzz.py (seed 8310): a match on four or more boxed integer
   constants is a test tree (Matching.split_sequence), whose `<` test and
   `=` test use the same pattern constant, so ocamlopt's exported body of
   an [@inline] function shares that constant's boxed integer in the .cmx
   (two Uconst_int64 blocks, one int64).  The port's Closure dropped the
   box's identity and wrote the integer twice. *)
let[@inline] f (x : int64) = match x with 1L -> 1 | 2L -> 2 | 3L -> 3 | 4L -> 4 | _ -> 0
let[@inline] g (x : int32) = match x with 10l -> 1 | 20l -> 2 | 30l -> 3 | 40l -> 4 | 50l -> 5 | _ -> 0
let[@inline] h (x : nativeint) = match x with 7n -> 1 | 8n -> 2 | 9n -> 3 | (11n | 12n) -> 4 | _ -> 0
let () = Printf.printf "%d %d %d\n" (f 3L + f 9L) (g 40l) (h 12n)
