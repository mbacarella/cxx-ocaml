module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = match 1 with 0 -> S.cardinal | _ -> S.cardinal
end
