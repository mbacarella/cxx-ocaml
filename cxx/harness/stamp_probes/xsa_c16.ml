module HW = Weak.Make(Int)
module F (X : Hashtbl.HashedType) = Hashtbl.Make(X)
