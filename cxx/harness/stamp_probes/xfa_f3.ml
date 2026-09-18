module F (X : Hashtbl.HashedType) = Hashtbl.Make (X)
module N = F (Int)
