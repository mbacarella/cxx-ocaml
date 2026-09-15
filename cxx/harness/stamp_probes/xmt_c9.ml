module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = [S.union; fun _ _ -> S.empty]
end
