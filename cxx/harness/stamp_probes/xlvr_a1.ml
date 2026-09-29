module F (X : Set.OrderedType) = struct
  module M = Map.Make(X)
  type 'a t = 'a M.t
end
