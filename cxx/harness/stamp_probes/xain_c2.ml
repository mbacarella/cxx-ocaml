(* control: a local structure alias *)
module A = struct let x = 1 end
module S = A
include S
