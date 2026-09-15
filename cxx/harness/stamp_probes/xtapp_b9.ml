module F (X : Set.OrderedType) = struct module rec M : sig type t = Set.Make(X).t end = struct type t = Set.Make(X).t end end
