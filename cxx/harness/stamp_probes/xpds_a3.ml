(* a local open reaches the name THROUGH the open, not by prefixing *)
module N = struct type t end
type u = N.(t)
