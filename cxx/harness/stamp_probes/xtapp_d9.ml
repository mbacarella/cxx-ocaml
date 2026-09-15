module F (X : Set.OrderedType) : sig type t = Set.Make(X).t end = struct module S = Set.Make(X) type t = S.t end
