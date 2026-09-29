module P = Set.Make(String)
module F (A : Set.OrderedType) : sig end = struct module S = Set.Make(A) end
