(* control: a shadowed value sets off no nondep *)
module X = struct type t = int end
include X
let t = 3
module Y = struct type z end
