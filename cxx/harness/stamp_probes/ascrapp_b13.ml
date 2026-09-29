module P = Set.Make(String)
module F (A : Set.OrderedType) = struct module S = Set.Make(A) end
