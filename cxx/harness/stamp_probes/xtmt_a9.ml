(* known stamp gap: the same path read at depth *)
module X = struct
  module Y = struct module Z = struct let a = 1 let b = 2 end let v = 1 end
end
module type S = module type of X.Y
