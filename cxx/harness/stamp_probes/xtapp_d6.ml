module F (X : Set.OrderedType) = struct type t = Set.Make(X).t end
module G (X : Set.OrderedType) = struct type t = Set.Make(X).t end
