open Effect
open Effect.Deep
type _ eff += Print : string -> unit eff | Get : int eff | Any : 'a -> 'a eff
let m1 = 0
let f1 f = match f () with () -> () | effect Print s, k -> continue k ()
let m2 = 0
let f2 f = match f () with () -> () | effect Print s, k -> continue k ()
  | effect Get, k -> continue k 1
let m3 = 0
let f3 f = match f () with () -> () | effect Any x, k -> continue k x
let m4 = 0
let f4 f = match f () with () -> () | effect Print s, k -> continue k ()
  | effect Get, _ -> ()
let m5 = 0
let f5 f = try f () with effect Print s, k -> continue k ()
let m6 = 0
let f6 f = match f () with () -> () | effect Print _, k -> continue k ()
  | effect Print _, k -> continue k ()
let m7 = 0
let f7 f = match f () with () -> () | effect Get, k -> continue k 1
  | effect Print s, k -> continue k ()
let m8 = 0
let f8 f = match f () with () -> () | effect Get, k -> continue k 1
  | effect Print s, k -> continue k () | effect Any x, k -> continue k x
let m9 = 0
let f9 f = match f () with () -> ()
  | effect (Get | Print _), k -> discontinue k Not_found
let m10 = 0
let f10 f = match f () with () -> () | effect Print s, k -> continue k ()
  | effect Print "a", k -> continue k ()
let m11 = 0
