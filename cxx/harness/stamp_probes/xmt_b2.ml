module M : sig type u end = struct
  module S = Hashtbl.Make (String)
  type u = int
  let y = [S.create; S.create]
end
