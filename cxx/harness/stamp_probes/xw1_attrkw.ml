(* attribute ids may be keywords (single_attr_id), in every dotted segment *)
let x = 1
[@@@end]
[@@@deriving.end]
let y = 2 [@@and.or]
let z = (3 [@if.then])
