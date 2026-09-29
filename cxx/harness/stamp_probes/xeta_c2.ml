let id x = x
let f1 ?(x = 1) (y : int) = ignore x; ignore y
let h = id f1
