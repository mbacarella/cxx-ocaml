module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = match 1 with _ -> S.cardinal
end
