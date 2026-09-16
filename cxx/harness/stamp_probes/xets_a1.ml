let f1 ?x y = ignore x; ignore y
type r = { f : int -> unit; g : int }
let r0 = { f = (fun _ -> ()); g = 1 }
let h = { r0 with f = f1 }
