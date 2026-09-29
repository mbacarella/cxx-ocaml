module F (X : sig module N : Set.OrderedType end) = struct
  let v = X.N.compare
end
