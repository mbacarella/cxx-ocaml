module H = Hashtbl.Make (struct
  type t = int
  let equal (a : int) b = a = b
  let hash (x : int) = x
end)
let t : int H.t = H.create 4
