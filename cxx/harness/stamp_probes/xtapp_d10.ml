module F (X : Set.OrderedType) = struct type t = Set.Make(X).t end
module type T = sig type t end
