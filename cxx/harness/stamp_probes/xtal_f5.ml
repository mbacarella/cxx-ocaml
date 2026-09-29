module F (X : Set.OrderedType) = struct
  module type T = sig val v : Set.Make(X).t end
end
