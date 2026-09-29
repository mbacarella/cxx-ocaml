module F (X : Set.OrderedType) : sig type t = Set.Make(X).t end = struct type t = Set.Make(X).t end
