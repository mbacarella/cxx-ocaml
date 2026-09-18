module F (X : Set.OrderedType) = Set.Make (X)
module G (Y : Set.OrderedType) = struct module N = F (Y) end
module M = G (Int)
