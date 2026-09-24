(* an external alias takes its OWN attributes *)
external f : int -> int = "f" [@@noalloc]
external g : int -> int = "%identity" [@@noalloc]
