open Effect
open Effect.Deep
type _ eff += L : 'a list -> 'a list eff | P : 'a * 'b -> ('a * 'b) eff
type _ eff += Q : 'a -> 'a option eff
let m1 = 0
let f1 f = match f () with () -> () | effect L l, k -> continue k l
let m2 = 0
let f2 f = match f () with () -> () | effect P (a, b), k -> continue k (a, b)
let m3 = 0
let f3 f = match f () with
  | () -> ()
  | effect L l, k -> continue k l
  | effect P (a, b), k -> continue k (a, b)
  | effect Q x, k -> continue k (Some x)
let m4 = 0
let f4 f = match f () with
  | () -> ()
  | effect (L _ | Q _), k -> discontinue k Not_found
let m5 = 0
