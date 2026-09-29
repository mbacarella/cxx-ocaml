module P (B : Set.OrderedType) = Set.Make (struct include B end)
