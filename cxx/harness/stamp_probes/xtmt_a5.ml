(* known stamp gap: a module type of a local dotted path *)
module X = struct module Y = struct let v = 1 type t = int end end
module type S = module type of X.Y
