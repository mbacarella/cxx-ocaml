module F (X : Hashtbl.HashedType) = struct
  module H : Hashtbl.S = Hashtbl.Make (X)
  let z = 0
end
