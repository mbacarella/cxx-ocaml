module M : sig val y : int end = struct
  module S = Set.Make (String)
  type v = S.t
  let y = 1
end
