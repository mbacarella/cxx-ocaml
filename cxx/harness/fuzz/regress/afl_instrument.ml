(* FUZZ: levels=nat,fl flags=nat:-afl-instrument nat:-afl-inst-ratio nat:60 fl:-O3 fl:-afl-instrument
   Found by fuzz.py (seed 88, the new nat:-afl-instrument flag set): the
   port ignored -afl-instrument.  Cmmgen instruments every function body
   and the unit's initialiser (asmcomp/afl_instrument.ml: a logging
   sequence at every branch target, its location drawn from the stdlib's
   default Random state, which runs on across functions and files), and
   Translclass skips the method cache.  -afl-inst-ratio draws a Random.int
   100 per site first. *)
class c (x : int) = object (self)
  val mutable v = x
  method get = v
  method bump k = v <- v + k; self#get
end
class d x = object inherit c x as super method! bump k = super#bump (2 * k) end
exception E of int
let classify n = match n land 7 with 0 -> "a" | 1 | 2 -> "b" | 3 -> "c" | _ -> "d"
let f b n =
  if b then (new d n)#bump 4
  else try if n > 3 then raise (E n) else (new c n)#get with E k -> k * 2
let rec loop i acc = if i = 0 then acc else loop (i - 1) (acc + f (i land 1 = 0) i)
let () =
  print_endline (classify (loop 10 0));
  print_endline (String.concat "," (List.map classify [1; 2; 3; 8]))
