(* a nested include of a unit alias *)
module M = struct module S = List include S end
let z = 1
