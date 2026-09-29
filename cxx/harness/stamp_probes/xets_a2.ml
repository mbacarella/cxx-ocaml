let f1 ?x y = ignore x; ignore y
type r = { mutable f : int -> unit }
let r0 = { f = (fun _ -> ()) }
let () = r0.f <- f1
