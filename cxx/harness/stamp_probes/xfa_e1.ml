module F (X : Set.OrderedType) = Set.Make (X)
module M = struct module N = F (Int) end
