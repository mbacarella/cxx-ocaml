(* control: only types after the shadow *)
include struct type t = int end
type t = float
let x = 1
let y = 2
