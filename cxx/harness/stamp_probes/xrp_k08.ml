module P = struct module MyMap(X : Hashtbl.HashedType) = X end
module N = P.MyMap(Int)
