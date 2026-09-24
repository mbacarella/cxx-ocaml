(* an external keeps its [@@noalloc] *)
external f : int -> int = "f" [@@noalloc]
