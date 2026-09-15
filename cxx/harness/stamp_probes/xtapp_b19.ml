module F (X : Set.OrderedType) = struct type t = MoreLabels.Set.Make(X).t end
