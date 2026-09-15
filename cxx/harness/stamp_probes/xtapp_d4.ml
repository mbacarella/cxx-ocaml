module F (X : Set.OrderedType) = struct type t = Set.Make(Set.Make(X)).t end
