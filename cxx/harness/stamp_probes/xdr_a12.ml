module M : sig type u end = struct
  module S = Set.Make (String)
  module R = Map.Make (String)
  type v = S.t * int R.t
  type u = int
end
