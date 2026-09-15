module F (X : Set.OrderedType) = struct type t = X.t let y = 0 end
module M = struct type t = int let compare = compare end
module N : sig end = struct module S = F (M) end
