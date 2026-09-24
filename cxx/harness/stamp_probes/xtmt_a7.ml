(* control: a module type of a local module *)
module X = struct let v = 1 type t = int end
module type S = module type of X
