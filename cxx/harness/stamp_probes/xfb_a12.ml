module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
end
module G (Y : Map.OrderedType) = struct
  module T : Map.S = Map.Make (Y)
end
