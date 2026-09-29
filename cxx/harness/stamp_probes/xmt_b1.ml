module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = [S.add; S.add]
end
