module P = struct
  module MyMap(X : Hashtbl.HashedType) = struct include X end
end
module N = P.MyMap(Int)
