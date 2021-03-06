### =========================================================================
### GmapGenome class
### -------------------------------------------------------------------------
###
### Database of reference sequence used by the GMAP suite.
###

setClass("GmapGenome", representation(name = "character",
                                      directory = "GmapGenomeDirectory"))

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Accessors
###

directory <- function(x) {
  retVal <- NULL
  if (!is.null(x)) {
    retVal <- x@directory ## 'dir' is already taken
  }
  return(retVal)
}

setMethod("genome", "GmapGenome", function(x) x@name)

setMethod("path", "GmapGenome",
          function(object) file.path(path(directory(object)), genome(object)))

mapsDirectory <- function(x) {
  file.path(path(x), paste(genome(x), "maps", sep = "."))
}

setMethod("seqinfo", "GmapGenome", function(x) {
  if (!.gmapGenomeCreated(x))
    stop("GmapGenome index '", genome(x), "' does not exist")
  suppressWarnings({ # warning when colClasses is too long, even when fill=TRUE!
    tab <- read.table(.get_genome(path(directory(x)), genome(x),
                                  chromosomes = TRUE),
                      colClasses = c("character", "NULL", "integer",
                        "character"),
                      fill = TRUE)
  })
  Seqinfo(tab[,1], tab[,2], nzchar(tab[,3]), genome = genome(x))
})

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Constructor
###

setGeneric("genomeName", function(x) standardGeneric("genomeName"))

setMethod("genomeName", "character", function(x) x)
setMethod("genomeName", "BSgenome", function(x) providerVersion(x))
setMethods("genomeName", list("RTLFile", "RsamtoolsFile"),
           function(x) file_path_sans_ext(basename(path(x)), TRUE))
setMethod("genomeName", "ANY", function(x) {
    if (hasMethod("seqinfo", class(x))) {
        ans <- unique(genome(x))
        if (length(ans) > 1L) {
            stop("genome is ambiguous")
        }
        ans
    } else {
        stop("cannot derive a genome name")
    }
})

file_path_is_absolute <- function(x) {
  ## hack that is unlikely to work on e.g. Windows
  identical(substring(x, 1, 1), .Platform$file.sep)
}

file_path_is_dir <- function(x) {
  isTRUE(file.info(x)[,"isdir"])
}

GmapGenome <- function(genome,
                       directory = GmapGenomeDirectory(create = create), 
                       name = genomeName(genome), create = FALSE, ...)
{
  if (!isTRUEorFALSE(create))
    stop("'create' must be TRUE or FALSE")
  if (isSingleString(genome) && file_path_is_dir(genome)) {
    genome <- path.expand(genome)
    if (file_path_is_absolute(genome)) {
      if (!missing(directory))
        stop("'directory' should be missing when 'genome' is an absolute path")
      directory <- dirname(genome)
      genome <- basename(genome)
    }
  }
  if (isSingleString(directory))
    directory <- GmapGenomeDirectory(directory, create = create)
  if (is(genome, "DNAStringSet")) {
    if (missing(name))
      stop("If the genome argument is a DNAStringSet object",
           "the name argument must be provided")
    if (is.null(names(genome))) {
      stop("If the genome is provided as a DNAStringSet, ",
           "the genome needs to have names. ",
           "E.g., \"names(genome) <- someSeqNames")
    }
  }
  if (!isSingleString(name))
    stop("'name' must be a single, non-NA string")
  if (!is(directory, "GmapGenomeDirectory"))
    stop("'directory' must be a GmapGenomeDirectory object or path to one")
  db <- new("GmapGenome", name = name, directory = directory)
  if (create) {
    if (name %in% genome(directory))
      message("NOTE: genome '", name, "' already exists, not overwriting")
    else referenceSequence(db, ...) <- genome
  }
  db
}

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Building
###

setReplaceMethod("referenceSequence",
                 signature(x = "GmapGenome", value = "ANY"),
                 function(x, name, ..., value)
                 {
                   gmap_build(value, x, ...)
                 })

setGeneric("snps<-", function(x, name, ..., value) standardGeneric("snps<-"))

setReplaceMethod("snps", c("GmapGenome", "ANY"),
                 function(x, name, ..., value) {
                   snpDir <- GmapSnpDirectory(x)
                   snps(snpDir, name = name, genome = x,
                        iitPath = mapsDirectory(x), ...) <- value
                   x
                 })

setGeneric("spliceSites<-",
           function(x, ..., value) standardGeneric("spliceSites<-"))

setReplaceMethod("spliceSites", c("GmapGenome", "GRangesList"),
                 function(x, name, value) {
                   exonsFlat <- unlist(value, use.names=FALSE)
                   exonsPart <- PartitioningByWidth(value)
                   exonsHead <- exonsFlat[-end(exonsPart)]
                   donors <- flank(exonsHead, 1L, start = FALSE)
                   exonsTail <- exonsFlat[-start(exonsPart)]
                   acceptors <- flank(exonsTail, 1L, start = TRUE)
                   sites <- c(resize(donors, 2L, fix = "end"),
                              resize(acceptors, 2L, fix = "start"))
                   names(sites) <- values(sites)$exon_id
                   info <- rep(c("donor", "acceptor"), each = length(donors))
                   intronWidths <- abs(start(acceptors) - start(donors)) + 1L
                   info <- paste(info, intronWidths)
                   values(sites) <- DataFrame(info)
                   iit_store(sites, file.path(mapsDirectory(x), name))
                   x
                 })

#setReplaceMethod("spliceSites", c("GmapGenome", "TranscriptDb"),
setReplaceMethod("spliceSites", c("GmapGenome", "TxDb"),
                 function(x, name, value) {
                   spliceSites(x, name) <- exonsBy(value)
                   x
                 })

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Sequence access
###

setMethod("getSeq", "GmapGenome", function(x, which = seqinfo(x)) {
  if (!.gmapGenomeCreated(x))
      stop("Genome index does not exist")
  if (is.character(which)) {
      which <- seqinfo(x)[which]
  }
  which <- as(which, "GRanges")
  merge(seqinfo(x), seqinfo(which)) # for the checks
  ans <- .Call(R_Genome_getSeq, path(directory(x)), genome(x),
               as.character(seqnames(which)), start(which), width(which),
               as.character(strand(which)))
  if (!is.null(names(which)))
    names(ans) <- names(which)
  ans
})

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Coerce
###

setAs("ANY", "GmapGenome", function(from) GmapGenome(from))

setAs("GmapGenome", "DNAStringSet", function(from) {
  DNAStringSet(getSeq(from))
})

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Show
###

setMethod("show", "GmapGenome", function(object) {
  cat("GmapGenome object\ngenome:", genome(object), "\ndirectory:",
      path(directory(object)), "\n")
})

setMethod("showAsCell", "GmapGenome", function(object) genome(object))

### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Utilities
###

.normArgDb <- function(db, dir) {
  if (!is(db, "GmapGenome"))
    db <- GmapGenome(db, dir)
  db
}


.gmapGenomeCreated <- function(genome) {
  ##existance means the GENOME_NAME.chromosome exists
  
  d <- path(directory(genome))
  if (!file.exists(d))
    return(FALSE)

  chromosome.file <- paste(genome(genome), "chromosome", sep=".")
  possibleLoc1 <- file.path(d, chromosome.file)
  possibleLoc2 <- file.path(d, genome(genome), chromosome.file)
  if (!(file.exists(possibleLoc1) || file.exists(possibleLoc2)))
    return(FALSE)

  return(TRUE)
}

