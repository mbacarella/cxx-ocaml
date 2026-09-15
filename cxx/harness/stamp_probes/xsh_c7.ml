module F (X : Set.OrderedType) : Set.S with type elt = X.t = struct
  include Set.Make (X)
end
