module F (X : Set.OrderedType) = Set.Make (X)
module M = struct module N = F (Int) end
module M2 = struct module N = F (Int) end
