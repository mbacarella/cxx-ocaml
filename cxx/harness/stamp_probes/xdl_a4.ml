module N : sig end = struct
  module S = Set.Make (String)
  module T = Map.Make (String)
end
