module F (X : Hashtbl.HashedType) = struct
  module S : Hashtbl.S with type key = X.t = Hashtbl.Make (X)
  let z = 0
end
